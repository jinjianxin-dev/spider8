/*
 * Spider8 运动管理层实现
 *
 * 任务划分（两个任务都跑 20ms 周期，用 vTaskDelayUntil 而不是 vTaskDelay）：
 *
 *   motion_task   prio 5  算 8 个关节角 → 覆盖式写入队列
 *   servo_out_task prio 7  从队列取最新目标 → 写 LEDC
 *
 * 为什么必须 vTaskDelayUntil：vTaskDelay 是"等 20ms"，处理耗时会被叠加进周期，
 * 步态节奏会越跑越慢且抖。vTaskDelayUntil 是"到下个 20ms 边界"，节拍恒定 ——
 * 这对没有位置反馈的开环舵机是硬要求，节奏一散，脚就绊。
 *
 * 队列用长度 1 + xQueueOverwrite：永远是"最新目标"，天然丢弃过时指令，
 * 不会因为消费慢而积压出越来越大的延迟。
 */
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "motion.h"
#include "servo.h"

static const char *TAG = "motion";

/* 语音前端（AFE 连续音频流）全部绑 core0，伺服节拍必须绑 core1，
 * 否则 20ms 的舵机节奏会被语音任务抢掉，脚会抖甚至绊。
 * 若将来为兼容 esp-sr 降成单核（CONFIG_FREERTOS_UNICORE），core1 不存在，
 * 一律回落 core0，避免 xTaskCreatePinnedToCore 触发 assert 无限重启。 */
#if defined(CONFIG_FREERTOS_UNICORE)
#define SPIDER8_CORE_MOTION 0
#else
#define SPIDER8_CORE_MOTION 1
#endif

#define SPI_PI          3.14159265358979f
#define SERVO_TICK_MS   GAIT_TICK_MS

/* 姿态过渡状态：机身高度只由「膝角」唯一决定，髋角每拍由 IK 反解。
 * 详见 config.h 的 POSE_TRANS_MS_* 说明。 */
static float    s_pose_knee;         /* 当前插值中的膝角 */
static float    s_pose_knee_from;    /* 本次过渡起点膝角 */
static float    s_pose_amp;          /* 当前单腿偏移幅度系数 0~1（握手/挥手用） */
static float    s_pose_amp_from;
static float    s_pose_amp_to;
static uint32_t s_pose_elapsed_ms;   /* 本次过渡已用时间 */
static uint32_t s_pose_trans_ms = POSE_TRANS_MS_DEFAULT;

typedef struct {
    float joint_deg[J_COUNT];
    bool  cal_mode;
} servo_target_t;

/* ---- 状态 ---- */
static float          s_out[J_COUNT];
static motion_mode_t  s_mode;
static gait_params_t  s_gait;
static uint32_t       s_tick;
static pose_id_t      s_pose;
static uint32_t       s_pose_ms;
static float          s_manual[J_COUNT];

/* ---- 动作切换：一律经「中间态站姿」 ----
 * 请求进来先挂起，收脚回站姿后自动执行；中途又下命令只改挂起目标，
 * 不重启已经跑了一半的收脚过程。详见 config.h 的 GAIT_FADE_MS_* 说明。 */
typedef enum {
    TRANS_NONE = 0,
    TRANS_TO_STAND,       /* 淡出到中立站姿 */
    TRANS_SETTLE,         /* 停在中立站姿，等输出收敛 + 机械停稳 */
    TRANS_FROM_STAND      /* 从站姿淡入目标步态 */
} trans_phase_t;

typedef enum {
    REQ_NONE = 0,         /* 停在站姿（stop / pose stand） */
    REQ_POSE,
    REQ_WALK
} req_type_t;

static trans_phase_t  s_trans;
static req_type_t     s_req;
static pose_id_t      s_req_pose;
static uint8_t        s_req_pattern;
static int            s_req_speed;        /* -1 = 不改 speed */
static motion_mode_t  s_trans_src;        /* 进入 TO_STAND 时来自哪种模式 */
static uint32_t       s_trans_ms;         /* 本次过渡已用时间 */
static uint32_t       s_trans_dur_ms;     /* 进入过渡时冻结的时长，中途改 posedur/fadedur 不影响本次 */
static float          s_fade;             /* 0 = 纯站姿，1 = 满步幅 */
static float          s_fade_from;        /* TO_STAND 起点 fade */
static uint32_t       s_fade_dur_ms = GAIT_FADE_MS_DEFAULT;
/* 站姿停留：hold 是在线可调值，dur 是进入本次停留时冻结的值（中途改不打断） */
static uint32_t       s_settle_hold_ms  = GAIT_SETTLE_MS_DEFAULT;
static uint32_t       s_settle_dur_ms   = GAIT_SETTLE_MS_DEFAULT;
static uint32_t       s_settle_ms;          /* 本次停留已用时间 */
static uint8_t        s_settle_ok_beats;    /* 连续「输出已收敛」的拍数 */
/* 淡出期间的参数快照：中途拖动 speed/turn 滑块不能污染正在收脚的动作 */
static gait_params_t  s_trans_params;

/* ---- 静态分配（项目规范：运行期禁止动态分配） ---- */
static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;
static uint8_t       s_q_storage[sizeof(servo_target_t)];

static StaticTask_t  s_motion_tcb;
static StackType_t   s_motion_stack[MOTION_TASK_STACK];

static StaticTask_t  s_servo_tcb;
static StackType_t   s_servo_stack[SERVO_TASK_STACK];

static TaskHandle_t  s_motion_h;
static TaskHandle_t  s_servo_h;

/* ------------------------------------------------------------------ */
/* 姿态解算                                                            */
/* ------------------------------------------------------------------ */

/* smoothstep：首尾角速度都为 0，起步/刹车不冲击舵机齿轮。
 * POSE 膝角缓动与 gait 淡入淡出共用同一条曲线。 */
static float smoothstep01(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* 状态机实现放在 pose_begin() 之后（它要调用 pose_begin），这里先声明 */
static void transition_tick(void);

void motion_stand_angles(float out[J_COUNT])
{
    float hip[LEG_COUNT], knee[LEG_COUNT];
    /* 上电第一拍就摆到「默认站姿」，不是装配基准。
     * servo_init 的 s_ref_joint 仍用 GAIT_STAND_KNEE_DEG(-38°) 当零点，
     * 所以这里的 -48° 会让舵机真的多转 10°，而不是只改个名字。 */
    gait_solve_static(GAIT_DEFAULT_STAND_KNEE_DEG, NULL, NULL, hip, knee);

    for (int l = 0; l < LEG_COUNT; l++) {
        out[l * 2]     = hip[l];
        out[l * 2 + 1] = knee[l];
    }
}

/* 站姿膝角 + 增量，夹在软限位允许的最深处 */
static float pose_knee_delta(float dk)
{
    float k = s_gait.stand_knee_deg + dk;
    if (k < POSE_KNEE_FLOOR) k = POSE_KNEE_FLOOR;
    return k;
}

/* 位姿参数 → 目标膝角 + 四条腿的脚偏移（不含时间插值）
 *
 * 关键点：机身高度由屈膝角唯一决定，脚永远解到地面。
 * 所以蹲下/趴下不是"把腿角改小"，而是"把身子降下来、腿自动折叠"。
 *
 * 【必须只返回一个膝角标量】两条腿等长时髋角恒等于 −膝角/2，脚才在髋点
 * 正下方。旧版直接给 8 个关节角、再逐关节等速限速，会破坏这个 1:2 比例，
 * 下降过程中四只脚在地面上前后滑 18mm —— 现在由 motion_tick 先对膝角
 * 做缓动、再调 gait_solve_static 一次性重解 8 路，比例恒成立。 */
static void pose_target(pose_id_t id, uint32_t ms,
                        float *knee_deg,
                        float foot_y[LEG_COUNT], float foot_lift[LEG_COUNT])
{
    for (int l = 0; l < LEG_COUNT; l++) {
        foot_y[l]    = 0.0f;
        foot_lift[l] = 0.0f;
    }
    *knee_deg = s_gait.stand_knee_deg;

    switch (id) {
    case POSE_CROUCH:
        *knee_deg = pose_knee_delta(POSE_CROUCH_DK);
        break;

    case POSE_LIE:
        *knee_deg = pose_knee_delta(POSE_LIE_DK);
        break;

    case POSE_SHAKE:
        /* 2-DOF 做不出真正的横向伸爪，实际是"抬起 + 前伸"，
         * 视觉上像伸手，方向朝前。要真侧向递爪得加第三个关节。 */
        foot_lift[LEG_FR] = 34.0f;
        foot_y[LEG_FR]    = 22.0f;
        break;

    case POSE_WAVE: {
        /* 1.2Hz 上下摆：30±10mm 抬脚 */
        const float w = 2.0f * SPI_PI * 1.2f * (float)ms / 1000.0f;
        foot_lift[LEG_FR] = 30.0f + 10.0f * sinf(w);
        foot_y[LEG_FR]    = 20.0f;
        break;
    }

    case POSE_STAND:
    default:
        break;
    }
}

static void gait_output(uint32_t tick, float out[J_COUNT])
{
    /* 淡出必须用「进入过渡那一刻」的参数快照：用户可能在收脚途中拖 speed
     * 滑块或按 left/right，实时 s_gait 已变成下一动作的参数，直接拿来淡出
     * 会让步幅瞬间从 100% 掉到 0%（真跳变）。淡入则相反，要用新参数。 */
    gait_params_t p;
    const gait_params_t *src =
        (s_trans == TRANS_TO_STAND) ? &s_trans_params : &s_gait;
    gait_apply_fade(src, s_fade, &p);

    uint32_t tpc = (uint32_t)p.period_ms / GAIT_TICK_MS;
    if (tpc < 2) {
        tpc = 2;
    }

    gait_out_t g;
    gait_step(&p, tick % tpc, tpc, &g);

    for (int l = 0; l < LEG_COUNT; l++) {
        out[l * 2]     = g.hip_deg[l];
        out[l * 2 + 1] = g.knee_deg[l];
    }
}

/* 斜率限制：每拍每个关节最多走 11°（550°/s）。
 * 默认步态峰值是 455°/s，不会碰到；只有模式切换时才会被它拉平。 */
static void rate_limit(float io[J_COUNT], const float tgt[J_COUNT])
{
    const float step = MOTION_SLEW_DPS * ((float)GAIT_TICK_MS / 1000.0f);

    for (int j = 0; j < J_COUNT; j++) {
        float d = tgt[j] - io[j];
        if (d >  step) d =  step;
        if (d < -step) d = -step;
        io[j] += d;
    }
}

/* ------------------------------------------------------------------ */
/* 单拍逻辑                                                            */
/* ------------------------------------------------------------------ */

static void motion_tick(void)
{
    /* 先推进切换状态机：它可能在这一拍完成「回站姿」并立刻切到挂起的目标 */
    transition_tick();

    servo_target_t tgt;

    if (s_mode == MODE_MANUAL) {
        /* 标定：不走斜率限制（否则点动一个关节要等好几拍），
         * 只靠 servo_apply 里的 12°/拍 保命闸兜底。 */
        memcpy(s_out, s_manual, sizeof(s_out));
        memcpy(tgt.joint_deg, s_manual, sizeof(tgt.joint_deg));
        tgt.cal_mode = true;
        xQueueOverwrite(s_q, &tgt);
        s_tick++;
        return;
    }

    float raw[J_COUNT];

    if (s_mode == MODE_WALK) {
        gait_output(s_tick, raw);
        s_tick++;
    } else {
        s_pose_ms += GAIT_TICK_MS;
        if (s_pose_elapsed_ms < s_pose_trans_ms) {
            s_pose_elapsed_ms += GAIT_TICK_MS;
        }
        float t = (float)s_pose_elapsed_ms / (float)s_pose_trans_ms;
        /* smoothstep：首尾角速度都趋于 0，起步和刹车不冲击舵机齿轮。
         * 峰值斜率 1.5 → 站→趴(57°) 在 900ms 下约 95°/s，远低于 600°/s。 */
        const float e = smoothstep01(t);

        float knee_tgt, fy_tgt[LEG_COUNT], fl_tgt[LEG_COUNT];
        pose_target(s_pose, s_pose_ms, &knee_tgt, fy_tgt, fl_tgt);

        s_pose_knee = s_pose_knee_from + (knee_tgt - s_pose_knee_from) * e;
        s_pose_amp  = s_pose_amp_from  + (s_pose_amp_to - s_pose_amp_from) * e;

        float fy[LEG_COUNT], fl[LEG_COUNT];
        for (int l = 0; l < LEG_COUNT; l++) {
            fy[l] = fy_tgt[l] * s_pose_amp;
            fl[l] = fl_tgt[l] * s_pose_amp;
        }

        /* 髋角由 IK 反解，恒等于 −膝角/2 → 脚前后位移恒为 0，机身纯垂直升降 */
        float hip[LEG_COUNT], knee[LEG_COUNT];
        gait_solve_static(s_pose_knee, fy, fl, hip, knee);
        for (int l = 0; l < LEG_COUNT; l++) {
            raw[l * 2]     = hip[l];
            raw[l * 2 + 1] = knee[l];
        }
    }

    rate_limit(s_out, raw);

    memcpy(tgt.joint_deg, s_out, sizeof(tgt.joint_deg));
    tgt.cal_mode = false;
    xQueueOverwrite(s_q, &tgt);
}

/* ------------------------------------------------------------------ */
/* 任务                                                                */
/* ------------------------------------------------------------------ */

static void motion_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        motion_tick();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(GAIT_TICK_MS));
    }
}

static void servo_out_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    servo_target_t t;

    for (;;) {
        /* Peek 而不是 Receive：拿最新目标但不消费，
         * 万一 motion_task 卡住，这里仍然按最后目标保持姿态（安全）。 */
        if (xQueuePeek(s_q, &t, 0) == pdTRUE) {
            servo_apply(t.joint_deg, t.cal_mode);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SERVO_TICK_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t motion_init(void)
{
    s_gait.period_ms      = 1200;
    s_gait.duty_pct       = 25;
    s_gait.stride_mm      = 22.0f;
    /* 抬脚 14mm 不是拍脑袋：抬 16mm 时峰值角速度会到 516°/s，
     * 吃掉 MG90S 600°/s 的 86%，一点余量都不剩。14mm 降到 ~451°/s，
     * 留 25% 余量；离地 14mm 对 100mm 高的机身足够跨过接缝。
     * 腿弯一点后 dh/dknee 变大，同样抬脚所需角度反而更小，更安全。 */
    s_gait.lift_mm        = 14.0f;
    s_gait.stand_knee_deg = GAIT_DEFAULT_STAND_KNEE_DEG;
    s_gait.speed_pct      = 100;
    s_gait.turn_pct       = 0;
    s_gait.pattern        = GAIT_PATTERN_WALK;

    s_q = xQueueCreateStatic(1, sizeof(servo_target_t), s_q_storage, &s_q_buf);
    if (s_q == NULL) {
        ESP_LOGE(TAG, "目标队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_mode    = MODE_POSE;
    s_pose    = POSE_STAND;
    s_tick    = 0;
    s_pose_ms = 0;

    /* 上电即「稳定站姿」：这样第一条命令不用先等一次多余的收脚 */
    s_trans        = TRANS_NONE;
    s_req          = REQ_NONE;
    s_req_speed    = -1;
    s_trans_src    = MODE_POSE;
    s_trans_ms     = 0;
    s_fade         = 0.0f;
    s_fade_from    = 0.0f;
    s_fade_dur_ms     = GAIT_FADE_MS_DEFAULT;
    s_settle_hold_ms  = GAIT_SETTLE_MS_DEFAULT;
    s_settle_dur_ms   = GAIT_SETTLE_MS_DEFAULT;
    s_settle_ms       = 0;
    s_settle_ok_beats = 0;
    memset(&s_trans_params, 0, sizeof(s_trans_params));

    /* 上电就是站姿：过渡记为"已完成"，避免开机先播一次 900ms 的蹲起 */
    s_pose_trans_ms  = POSE_TRANS_MS_DEFAULT;
    s_pose_knee      = GAIT_DEFAULT_STAND_KNEE_DEG;
    s_pose_knee_from = GAIT_DEFAULT_STAND_KNEE_DEG;
    s_pose_amp       = 0.0f;
    s_pose_amp_from  = 0.0f;
    s_pose_amp_to    = 0.0f;
    s_pose_elapsed_ms = s_pose_trans_ms;

    motion_stand_angles(s_out);
    memcpy(s_manual, s_out, sizeof(s_manual));

    return ESP_OK;
}

esp_err_t motion_start(void)
{
    TaskHandle_t h;

    h = xTaskCreateStaticPinnedToCore(servo_out_task, "servo_out", SERVO_TASK_STACK, NULL,
                                      SERVO_TASK_PRIO, s_servo_stack, &s_servo_tcb,
                                      SPIDER8_CORE_MOTION);
    if (h == NULL) {
        ESP_LOGE(TAG, "servo_out 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    s_servo_h = h;

    h = xTaskCreateStaticPinnedToCore(motion_task, "motion", MOTION_TASK_STACK, NULL,
                                      MOTION_TASK_PRIO, s_motion_stack, &s_motion_tcb,
                                      SPIDER8_CORE_MOTION);
    if (h == NULL) {
        ESP_LOGE(TAG, "motion 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    s_motion_h = h;

    ESP_LOGI(TAG, "运动任务启动：motion@%dms prio%d / servo_out@%dms prio%d",
             GAIT_TICK_MS, MOTION_TASK_PRIO, SERVO_TICK_MS, SERVO_TASK_PRIO);
    return ESP_OK;
}

/* 姿态切换：起点取"当前实际姿态"，所以中途打断（趴到一半敲 stand）也平滑。
 * walk 模式下四腿相位不同，取膝角平均值作为等效起点；剩余偏差由
 * rate_limit 的 11°/拍 兜底拉平。 */
static void pose_begin(pose_id_t id)
{
    if (s_mode == MODE_POSE) {
        s_pose_knee_from = s_pose_knee;
        s_pose_amp_from  = s_pose_amp;
    } else {
        s_pose_knee_from = (s_out[J_FL_KNEE] + s_out[J_FR_KNEE] +
                            s_out[J_RL_KNEE] + s_out[J_RR_KNEE]) * 0.25f;
        s_pose_amp_from  = 0.0f;
    }
    s_pose_knee      = s_pose_knee_from;
    s_pose_amp       = s_pose_amp_from;
    s_pose_amp_to    = (id == POSE_SHAKE || id == POSE_WAVE) ? 1.0f : 0.0f;
    s_pose_elapsed_ms = 0;
    s_pose_ms        = 0;
    s_pose           = id;
    s_mode           = MODE_POSE;
}

/* ------------------------------------------------------------------ */
/* 动作切换状态机：所有切换先经「中间态站姿」                              */
/* ------------------------------------------------------------------ */

/* 只有正稳定站在中立站姿，才能直接出发；否则必须先收脚 */
static bool at_stable_stand(void)
{
    if (s_trans != TRANS_NONE)                 return false;
    if (s_mode != MODE_POSE || s_pose != POSE_STAND) return false;
    if (s_pose_elapsed_ms < s_pose_trans_ms) return false;
    if (fabsf(s_pose_knee - s_gait.stand_knee_deg) > 0.5f) return false;
    if (fabsf(s_pose_amp) > 0.02f)           return false;
    return true;
}

/* 把 POSE 状态强制对齐到「站姿已完成」。此时 gait 淡出的输出本来就等于
 * 站姿几何（单测 Q2 已证），所以这不是跳变，只是把状态变量对齐。 */
static void mark_stand_settled(void)
{
    s_mode            = MODE_POSE;
    s_pose            = POSE_STAND;
    s_fade            = 0.0f;
    s_pose_elapsed_ms = s_pose_trans_ms;
    s_pose_knee       = s_gait.stand_knee_deg;
    s_pose_knee_from  = s_pose_knee;
    s_pose_amp        = 0.0f;
    s_pose_amp_from   = 0.0f;
    s_pose_amp_to     = 0.0f;
}

/* 实际输出 s_out 与「站姿几何」的最大关节角偏差。
 *
 * 为什么不能只看状态变量：mark_stand_settled() 改的是插值曲线与模式，
 * 真正送到舵机的是 s_out，它每拍被 rate_limit 限在 11° 以内。从趴下
 * (-95°) 或行走中淡出时，s_out 可能还落后目标几度；此刻若马上换成
 * 下一个动作的目标，两段角速度叠在一起，髋膝 1:2 又被破坏。
 * 这个函数就是「真正站稳」的判据。 */
static float stand_output_error(void)
{
    float hip[LEG_COUNT], knee[LEG_COUNT], stand[J_COUNT];
    gait_solve_static(s_gait.stand_knee_deg, NULL, NULL, hip, knee);
    for (int l = 0; l < LEG_COUNT; l++) {
        stand[l * 2]     = hip[l];
        stand[l * 2 + 1] = knee[l];
    }

    float err = 0.0f;
    for (int j = 0; j < J_COUNT; j++) {
        const float d = fabsf(s_out[j] - stand[j]);
        if (d > err) err = d;
    }
    return err;
}

static void execute_pending(void)
{
    const req_type_t r = s_req;
    s_req       = REQ_NONE;
    s_trans     = TRANS_NONE;
    s_trans_ms  = 0;

    switch (r) {
    case REQ_POSE:
        if (s_req_pose == POSE_STAND) {
            mark_stand_settled();        /* 到站即目标，不播零行程缓动 */
        } else {
            pose_begin(s_req_pose);      /* 从站姿起步，900ms 膝角缓动 */
            s_fade = 0.0f;
        }
        ESP_LOGI(TAG, "→ %s", motion_pose_name(s_req_pose));
        break;

    case REQ_WALK:
        if (s_req_speed >= 0) {
            motion_set_speed(s_req_speed);
        }
        if (s_req_pattern < GAIT_PATTERN_COUNT) {
            s_gait.pattern = s_req_pattern;
        }
        s_tick = 0;
        s_mode = MODE_WALK;
        s_fade = 0.0f;                  /* 起步首拍 = 纯站姿，相位清零不跳 */
        s_trans        = TRANS_FROM_STAND;  /* 再从 0 淡入到目标步幅 */
        s_trans_ms     = 0;
        s_trans_dur_ms = s_fade_dur_ms;
        ESP_LOGI(TAG, "→ 行走 %s speed=%d%% turn=%d%%（淡入 %lums）",
                 gait_pattern_name(s_gait.pattern), s_gait.speed_pct,
                 s_gait.turn_pct, (unsigned long)s_fade_dur_ms);
        break;

    case REQ_NONE:
    default:
        mark_stand_settled();
        break;
    }
}

static void begin_to_stand(void)
{
    if (s_trans == TRANS_TO_STAND) {
        return;      /* 已在收脚：挂起目标已更新，别重启计时造成来回拉扯 */
    }
    if (s_trans == TRANS_SETTLE) {
        /* 已经站在中立姿态、正在停留：只换挂起目标，不要重新播一次收脚。
         * 换成新动作时重新给一次完整稳定窗口；只是 stop 就不拖延急停。 */
        if (s_req != REQ_NONE) {
            s_settle_ms       = 0;
            s_settle_ok_beats = 0;
            ESP_LOGI(TAG, "站姿停留中改目标 → %s（重新计时 %lums）",
                     motion_pending_name(), (unsigned long)s_settle_dur_ms);
        }
        return;
    }
    s_trans_src    = s_mode;
    s_trans_ms     = 0;
    s_trans        = TRANS_TO_STAND;
    s_trans_dur_ms = (s_mode == MODE_WALK) ? s_fade_dur_ms : s_pose_trans_ms;

    if (s_mode == MODE_WALK) {
        s_trans_params = s_gait;         /* 快照旧参数，隔离中途的参数改动 */
        s_fade_from    = s_fade;
        ESP_LOGI(TAG, "→ 中间态站姿：步幅淡出 %lums",
                 (unsigned long)s_trans_dur_ms);
    } else {
        /* POSE / MANUAL：复用现有膝角 smoothstep 缓动，起点取当前实际膝角 */
        pose_begin(POSE_STAND);
        s_fade = 0.0f;
        ESP_LOGI(TAG, "→ 中间态站姿：膝角回位 %lums",
                 (unsigned long)s_trans_dur_ms);
    }
}

static void request_action(req_type_t r, pose_id_t pose, uint8_t pattern, int speed)
{
    s_req         = r;
    s_req_pose    = pose;
    s_req_pattern = pattern;
    s_req_speed   = speed;

    if (at_stable_stand()) {
        execute_pending();               /* 已在中立站姿，直接出发 */
    } else {
        begin_to_stand();                /* 先收脚，到站后自动执行挂起目标 */
    }
}

static void transition_tick(void)
{
    if (s_trans == TRANS_NONE) {
        return;
    }

    /* ---- 站姿停留：时间够了 + 输出真的追平站姿，两个条件都满足才放行 ---- */
    if (s_trans == TRANS_SETTLE) {
        s_settle_ms += GAIT_TICK_MS;

        const float err       = stand_output_error();
        const bool  converged = err < GAIT_SETTLE_TOL_DEG;
        s_settle_ok_beats = converged ? (uint8_t)(s_settle_ok_beats + 1) : 0;

        const bool time_ok = s_settle_ms >= s_settle_dur_ms;
        const bool mech_ok = s_settle_ok_beats >= GAIT_SETTLE_OK_BEATS;
        const bool guard   = s_settle_ms >= s_settle_dur_ms + GAIT_SETTLE_GUARD_MS;

        if ((time_ok && mech_ok) || guard) {
            if (guard && !mech_ok) {
                ESP_LOGW(TAG, "站姿停留超时：输出仍差 %.2f°，强制放行避免卡死",
                         (double)err);
            }
            s_settle_ms       = 0;
            s_settle_ok_beats = 0;
            execute_pending();
        }
        return;
    }

    uint32_t dur = s_trans_dur_ms;
    if (dur < GAIT_TICK_MS) {
        dur = GAIT_TICK_MS;
    }

    s_trans_ms += GAIT_TICK_MS;
    if (s_trans_ms > dur) s_trans_ms = dur;
    const float e = smoothstep01((float)s_trans_ms / (float)dur);

    if (s_trans == TRANS_FROM_STAND) {
        s_fade = e;
        if (s_trans_ms >= dur) {
            s_fade      = 1.0f;
            s_trans     = TRANS_NONE;
            s_trans_ms  = 0;
        }
        return;
    }

    /* TRANS_TO_STAND */
    if (s_trans_src == MODE_WALK) {
        s_fade = s_fade_from * (1.0f - e);
    }
    if (s_trans_ms >= dur) {
        mark_stand_settled();
        /* 关键改动：这里不再立刻执行挂起动作，先停一会儿等真正站稳。
         * 上一版「计时到 = 站稳」是错的：s_out 可能还落后、舵机还在转。 */
        s_trans           = TRANS_SETTLE;
        s_trans_ms        = 0;
        s_settle_ms       = 0;
        s_settle_ok_beats = 0;
        s_settle_dur_ms   = s_settle_hold_ms;  /* 冻结：中途改 standhold 不打断本次 */
        ESP_LOGI(TAG, "→ 站姿停留 %lums（输出需连续 %d 拍收敛到 %.1f° 内）",
                 (unsigned long)s_settle_dur_ms, GAIT_SETTLE_OK_BEATS,
                 GAIT_SETTLE_TOL_DEG);
    }
}

/* ---- 对外：一律「请求」式，不再硬切模式 ---- */

void motion_stand(void)
{
    s_gait.turn_pct = 0;     /* 站定就回正，不把转向偏置带进下一次起步 */
    request_action(REQ_NONE, POSE_STAND, s_gait.pattern, -1);
    ESP_LOGI(TAG, "→ 请求站立（经中间态站姿）");
}

void motion_pose(pose_id_t id)
{
    if (id >= POSE_COUNT) {
        return;
    }
    request_action(REQ_POSE, id, s_gait.pattern, -1);
    ESP_LOGI(TAG, "→ 请求 %s（经中间态站姿）", motion_pose_name(id));
}

void motion_walk(uint8_t pattern, int speed_pct)
{
    request_action(REQ_WALK, POSE_STAND, pattern, speed_pct);
    ESP_LOGI(TAG, "→ 请求行走 %s speed=%d",
             gait_pattern_name((uint8_t)pattern), speed_pct);
}

/* 原地左/右转：2-DOF 腿不能横向跨步，转向只能靠左右步幅差动。
 * speed=0 时旧公式把整条 stride 乘成 0，转向完全失效； gait.c 已把
 * 前进分量与自转分量拆开相加，speed=0/turn≠0 就能原地打转。 */
void motion_spin(int dir)
{
    motion_set_speed(0);
    motion_set_turn((dir < 0) ? -GAIT_SPIN_TURN_PCT : GAIT_SPIN_TURN_PCT);
    request_action(REQ_WALK, POSE_STAND, GAIT_PATTERN_WALK, -1);
    ESP_LOGI(TAG, "→ 原地%s转（差速 %d%%）",
             (dir < 0) ? "左" : "右", s_gait.turn_pct);
}

void motion_straight(void)
{
    motion_set_turn(0);
    motion_set_speed(100);
    request_action(REQ_WALK, POSE_STAND, s_gait.pattern, -1);
    ESP_LOGI(TAG, "→ 直行前进");
}

void motion_set_speed(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_gait.speed_pct = (int8_t)pct;
}

void motion_set_turn(int pct)
{
    if (pct < -100) pct = -100;
    if (pct >  100) pct =  100;
    s_gait.turn_pct = (int8_t)pct;
}

void motion_set_period(int ms)
{
    if (ms < 800)  ms = 800;
    if (ms > 3000) ms = 3000;
    s_gait.period_ms = (uint16_t)ms;
}

void motion_set_stride(float mm)
{
    if (mm < 8.0f)  mm = 8.0f;
    if (mm > 30.0f) mm = 30.0f;
    s_gait.stride_mm = mm;
}

void motion_set_lift(float mm)
{
    if (mm < 6.0f)  mm = 6.0f;
    if (mm > 26.0f) mm = 26.0f;
    s_gait.lift_mm = mm;
}

void motion_set_duty(int pct)
{
    if (pct < 10) pct = 10;
    if (pct > 45) pct = 45;
    s_gait.duty_pct = (uint8_t)pct;
}

void motion_set_pose_trans(int ms)
{
    if (ms < POSE_TRANS_MS_MIN) ms = POSE_TRANS_MS_MIN;
    if (ms > POSE_TRANS_MS_MAX) ms = POSE_TRANS_MS_MAX;
    s_pose_trans_ms = (uint32_t)ms;
    /* 过渡中途改时长：进度按比例重算，避免瞬间跳到终点或卡住不动 */
    if (s_pose_elapsed_ms > s_pose_trans_ms) {
        s_pose_elapsed_ms = s_pose_trans_ms;
    }
}

void motion_set_fade_trans(int ms)
{
    if (ms < GAIT_FADE_MS_MIN) ms = GAIT_FADE_MS_MIN;
    if (ms > GAIT_FADE_MS_MAX) ms = GAIT_FADE_MS_MAX;
    s_fade_dur_ms = (uint32_t)ms;
    /* 不碰 s_trans_dur_ms：正在跑的那次过渡用进入时冻结的时长走完，
     * 中途调 fadedur 只影响下一次切换，避免淡出被突然截断造成跳变。 */
}

void motion_set_settle_trans(int ms)
{
    if (ms < GAIT_SETTLE_MS_MIN) ms = GAIT_SETTLE_MS_MIN;
    if (ms > GAIT_SETTLE_MS_MAX) ms = GAIT_SETTLE_MS_MAX;
    s_settle_hold_ms = (uint32_t)ms;
    /* 不碰 s_settle_dur_ms：正在停留的这一次用进入时冻结的时长走完，
     * 中途调 standhold 只影响下一次切换，避免停留被突然截断。 */
}

/* 站立屈膝角：负值越大腿越弯、机身越低。
 * 只改「目标站姿」，装配基准 GAIT_STAND_KNEE_DEG 不动 ⇒ trim/dir 不用重标。 */
void motion_set_stand_knee(float deg)
{
    if (deg < GAIT_STAND_KNEE_MIN) deg = GAIT_STAND_KNEE_MIN;
    if (deg > GAIT_STAND_KNEE_MAX) deg = GAIT_STAND_KNEE_MAX;
    s_gait.stand_knee_deg = deg;

    /* 正站着时重新起步一次缓动：腿平滑弯下去，不会瞬跳。
     * 走步模式下一拍 gait_step 直接用新值，由 rate_limit 兜底。 */
    if (s_trans == TRANS_NONE && s_mode == MODE_POSE && s_pose == POSE_STAND) {
        s_pose_knee_from  = s_pose_knee;
        s_pose_elapsed_ms = 0;
    }
}

void motion_manual(int joint, float joint_deg)
{
    if (joint < 0 || joint >= J_COUNT) {
        return;
    }
    if (s_mode != MODE_MANUAL) {
        memcpy(s_manual, s_out, sizeof(s_manual));
        s_mode = MODE_MANUAL;
        /* 标定是人工点动，不能被后台过渡抢走模式；先取消挂起的动作 */
        s_trans           = TRANS_NONE;
        s_req             = REQ_NONE;
        s_fade            = 0.0f;
        s_settle_ms       = 0;
        s_settle_ok_beats = 0;
        ESP_LOGW(TAG, "→ 手动标定模式（不走路，只动手）");
    }
    s_manual[joint] = joint_deg;
}

/* ---- 查询 ---- */

const gait_params_t *motion_params(void) { return &s_gait; }
motion_mode_t        motion_mode(void)   { return s_mode; }
pose_id_t            motion_current_pose(void) { return s_pose; }
uint32_t             motion_tick_count(void)   { return s_tick; }
uint32_t             motion_pose_trans_ms(void) { return s_pose_trans_ms; }
uint32_t             motion_fade_trans_ms(void)   { return s_fade_dur_ms; }
uint32_t             motion_settle_trans_ms(void) { return s_settle_hold_ms; }

const char *motion_trans_name(void)
{
    switch (s_trans) {
    case TRANS_TO_STAND:   return "回站姿中";
    case TRANS_SETTLE:     return "站姿停留中";
    case TRANS_FROM_STAND: return "起步淡入中";
    case TRANS_NONE:
    default:               return "就绪";
    }
}

const char *motion_pending_name(void)
{
    if (s_trans != TRANS_TO_STAND && s_trans != TRANS_SETTLE) {
        return "—";
    }
    switch (s_req) {
    case REQ_POSE: return motion_pose_name(s_req_pose);
    case REQ_WALK: return gait_pattern_name(s_req_pattern);
    case REQ_NONE:
    default:       return "站立";
    }
}

const char *motion_mode_name(void)
{
    switch (s_mode) {
    case MODE_WALK:   return "walk";
    case MODE_MANUAL: return "manual";
    case MODE_POSE:
    default:          return "pose";
    }
}

const char *motion_pose_name(pose_id_t id)
{
    switch (id) {
    case POSE_CROUCH: return "蹲下";
    case POSE_LIE:    return "趴下";
    case POSE_SHAKE:  return "握手";
    case POSE_WAVE:   return "挥手";
    case POSE_STAND:
    default:          return "站立";
    }
}

void motion_get_out(float out[J_COUNT])
{
    if (out) {
        memcpy(out, s_out, sizeof(s_out));
    }
}

void motion_diag(uint32_t *motion_hwm_words, uint32_t *servo_hwm_words)
{
    if (motion_hwm_words) {
        *motion_hwm_words = s_motion_h ? (uint32_t)uxTaskGetStackHighWaterMark(s_motion_h) : 0;
    }
    if (servo_hwm_words) {
        *servo_hwm_words = s_servo_h ? (uint32_t)uxTaskGetStackHighWaterMark(s_servo_h) : 0;
    }
}
