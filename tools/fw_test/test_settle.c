/*
 * 「站姿停留」状态机 PC 回归单测 —— 跑的是真实的 firmware/main/motion.c
 *
 * 编译（项目根目录）：
 *   gcc -std=c11 -O2 -I tools/fw_test/stub -I firmware/main \
 *       tools/fw_test/test_settle.c firmware/main/gait.c -lm -o /tmp/test_settle
 *
 * 为什么要费劲把 motion.c 编进来：这次修的是【时序】bug，不是几何 bug。
 * 只看 gait.c 的纯函数证明不了「回站姿计时结束后到底有没有等站稳」。
 * 用桩把 FreeRTOS / ESP-IDF 替换掉、把 motion.c 直接 include 进同一编译单元，
 * 就能逐拍调用真实的 motion_tick()，并访问真实的 s_trans / s_out 静态状态。
 * 桩只是让代码能在桌面编译，被测逻辑一行没改。
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "config.h"
#include "gait.h"
#include "motion.h"
#include "servo.h"

/* 桩头要在定义桩函数之前引入，否则下面的类型还不可见 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

/* ---------- FreeRTOS / ESP 桩：与 RTOS 无关的部分先定义 ---------- */
static TickType_t g_ticks;

TickType_t xTaskGetTickCount(void) { return g_ticks; }
void vTaskDelayUntil(TickType_t *last, TickType_t inc)
{ (void)last; g_ticks += inc; }
uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t h) { (void)h; return 1024; }

QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t item,
                                 uint8_t *storage, StaticQueue_t *buf)
{
    (void)len; (void)item; (void)storage; (void)buf;
    return (QueueHandle_t)1;          /* 非 NULL，让 motion_init 通过 */
}
TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char *name,
                               uint32_t stack, void *param,
                               UBaseType_t prio, StackType_t *stack_buf,
                               StaticTask_t *tcb)
{
    (void)fn; (void)name; (void)stack; (void)param;
    (void)prio; (void)stack_buf; (void)tcb;
    return (TaskHandle_t)1;
}

/* ---------- 把真实 motion.c 编进本编译单元 ---------- */
#include "../../firmware/main/motion.c"

/* ---------- 依赖 motion.c 内部类型的桩 ---------- */
static servo_target_t g_q_target;      /* 「队列」里的最新目标 */
static float          g_applied[J_COUNT];
static int            g_apply_n;

BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{
    (void)q;
    memcpy(&g_q_target, item, sizeof(g_q_target));
    return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t q, void *buf, TickType_t ticks)
{
    (void)q; (void)ticks;
    memcpy(buf, &g_q_target, sizeof(g_q_target));
    return pdTRUE;
}
void servo_apply(const float joint_deg[J_COUNT], bool cal_mode)
{
    (void)cal_mode;
    memcpy(g_applied, joint_deg, sizeof(g_applied));
    g_apply_n++;
}

/* 一拍 = motion_task 算目标 + servo_out_task 下发，与板上 20ms 一致 */
static void beat(void)
{
    motion_tick();
    servo_apply(g_q_target.joint_deg, g_q_target.cal_mode);
    g_ticks += GAIT_TICK_MS;
}
static void beats(int n) { for (int i = 0; i < n; i++) beat(); }

/* ---------- 断言与工具 ---------- */
#define DEG2RAD (3.14159265358979f / 180.0f)
static int g_fail;

static void ck(int ok, const char *name)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

static void reset_fw(void)
{
    memset(&g_q_target, 0, sizeof(g_q_target));
    memset(g_applied, 0, sizeof(g_applied));
    g_apply_n = 0;
    g_ticks   = 0;
    motion_init();
    beat();                       /* 上电第一拍：队列里放一个有效站姿目标 */
}

/* 实际输出 s_out 与站姿几何的最大关节角偏差 —— 和固件里的判据同源 */
static float output_stand_error(void)
{
    float hip[LEG_COUNT], knee[LEG_COUNT], stand[J_COUNT], out[J_COUNT];
    gait_solve_static(motion_params()->stand_knee_deg, NULL, NULL, hip, knee);
    for (int l = 0; l < LEG_COUNT; l++) {
        stand[l * 2]     = hip[l];
        stand[l * 2 + 1] = knee[l];
    }
    motion_get_out(out);
    float e = 0.0f;
    for (int j = 0; j < J_COUNT; j++) {
        const float d = fabsf(out[j] - stand[j]);
        if (d > e) e = d;
    }
    return e;
}

/* 正运动学：脚相对髋点的前后位移。站姿时应恒为 0 */
static float max_foot_y(const float jd[J_COUNT])
{
    float m = 0.0f;
    for (int l = 0; l < LEG_COUNT; l++) {
        const float h = jd[l * 2]     * DEG2RAD;
        const float k = jd[l * 2 + 1] * DEG2RAD;
        const float y = GAIT_THIGH_MM * sinf(h) + GAIT_SHIN_MM * sinf(h + k);
        if (fabsf(y) > m) m = fabsf(y);
    }
    return m;
}

/* T1：趴下 → 走，必须「回站姿 → 停留 → 起步」三段齐全 */
static void t1_timeline(void)
{
    printf("\nT1 趴下 → 走：回站姿后必须停留，站稳才起步\n");
    reset_fw();
    ck(strcmp(motion_trans_name(), "就绪") == 0, "上电就绪，不播多余收脚");

    motion_pose(POSE_LIE);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS + 2);
    ck(motion_current_pose() == POSE_LIE, "趴下已完成");

    motion_walk(GAIT_PATTERN_WALK, 100);
    ck(strcmp(motion_trans_name(), "回站姿中") == 0, "趴→走先进回站姿");

    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS);
    printf("     回站姿计时结束 → %s / 模式 %s / 输出误差 %.4f°\n",
           motion_trans_name(), motion_mode_name(), output_stand_error());
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0,
       "计时结束后进入站姿停留，而不是立刻走（本次修复点）");
    ck(motion_mode() == MODE_POSE, "停留期间仍是 POSE，没有偷偷切到 WALK");
    ck(strcmp(motion_pending_name(), gait_pattern_name(GAIT_PATTERN_WALK)) == 0,
       "停留期间挂起的 walk 目标还在");

    const int hold_beats = GAIT_SETTLE_MS_DEFAULT / GAIT_TICK_MS;   /* 15 拍 */
    float max_err = 0.0f, max_y = 0.0f;
    for (int i = 1; i < hold_beats; i++) {
        beat();
        const float e = output_stand_error();
        const float y = max_foot_y(g_applied);
        if (e > max_err) max_err = e;
        if (y > max_y)   max_y = y;
    }
    printf("     停留前 %d 拍：输出最大偏差 %.4f° / 四脚最大前后位移 %.4fmm\n",
           hold_beats - 1, max_err, max_y);
    ck(max_err < GAIT_SETTLE_TOL_DEG, "停留期间输出恒等于站姿几何");
    ck(max_y < 0.02f, "停留期间四脚不前后滑（髋膝 1:2 保持）");
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "停留时间没到，不提前放行");
    ck(motion_mode() == MODE_POSE, "停留没到，目标动作仍未启动");

    beats(2);
    ck(motion_mode() == MODE_WALK, "停留结束后才真正开始走");
    ck(strcmp(motion_trans_name(), "起步淡入中") == 0, "起步从 0 步幅淡入");
}

/* T2：停留期间又下命令，只换目标，不能跳过停留 */
static void t2_change_target(void)
{
    printf("\nT2 停留期间改目标\n");
    reset_fw();
    motion_pose(POSE_LIE);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS + 2);
    motion_walk(GAIT_PATTERN_WALK, 100);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS);
    beats(3);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "换命令前正在停留");

    motion_pose(POSE_CROUCH);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "停留中改目标不会跳过停留");
    ck(motion_mode() == MODE_POSE, "仍未提前启动任何动作");
    ck(strcmp(motion_pending_name(), "蹲下") == 0, "挂起目标已换成蹲下");

    beats(GAIT_SETTLE_MS_DEFAULT / GAIT_TICK_MS + 3);
    ck(motion_current_pose() == POSE_CROUCH, "停留结束后执行的是新目标（不是旧 walk）");
    ck(motion_mode() == MODE_POSE, "确实没有偷偷开始走");
}

/* T3：输出一直不收敛时 guard 必须放行，不能把狗卡在站姿 */
static void t3_guard(void)
{
    printf("\nT3 输出始终不收敛时的超时兜底\n");
    reset_fw();
    motion_pose(POSE_LIE);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS + 2);
    motion_walk(GAIT_PATTERN_WALK, 100);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "已进入停留");

    const int limit = (GAIT_SETTLE_MS_DEFAULT + GAIT_SETTLE_GUARD_MS) / GAIT_TICK_MS + 10;
    int i = 0;
    for (; i < limit; i++) {
        beat();
        if (motion_mode() == MODE_WALK) break;
        /* 模拟舵机一直追不上：每拍把实际输出推离站姿 30°，
         * rate_limit 每拍只能拉回 11°，收敛条件永远不成立。 */
        for (int j = 0; j < J_COUNT; j++) s_out[j] += 30.0f;
    }
    printf("     第 %d 拍（%lums）放行\n", i + 1, (unsigned long)((i + 1) * GAIT_TICK_MS));
    ck(motion_mode() == MODE_WALK, "收敛不了也不卡死，guard 超时强制放行");
    ck((i + 1) * GAIT_TICK_MS <=
       GAIT_SETTLE_MS_DEFAULT + GAIT_SETTLE_GUARD_MS + 2 * GAIT_TICK_MS,
       "放行时间落在 guard 上限内");
}

/* T4：standhold 在线调参 + 时长冻结（中途改不打断本次） */
static void t4_standhold(void)
{
    printf("\nT4 standhold 在线调参与时长冻结\n");
    reset_fw();
    motion_set_settle_trans(800);
    ck(motion_settle_trans_ms() == 800, "standhold 800ms 生效");

    motion_pose(POSE_LIE);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS + 2);
    motion_walk(GAIT_PATTERN_WALK, 100);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "已进入停留");

    motion_set_settle_trans(GAIT_SETTLE_MS_MIN);   /* 中途改短 */
    beats(GAIT_SETTLE_MS_DEFAULT / GAIT_TICK_MS + 2);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0,
       "中途改 standhold 不打断本次停留（时长冻结）");

    beats(800 / GAIT_TICK_MS);
    ck(motion_mode() == MODE_WALK, "按冻结的 800ms 走完才放行");
}

/* T5：本来就已站稳，不该凭空多等一次停留 */
static void t5_direct(void)
{
    printf("\nT5 已站稳时直接出发\n");
    reset_fw();
    motion_walk(GAIT_PATTERN_WALK, 100);
    ck(motion_mode() == MODE_WALK, "上电稳定站姿 → 直接起步");
    ck(strcmp(motion_trans_name(), "起步淡入中") == 0, "只做淡入，不凭空多等 300ms");
    beats(GAIT_FADE_MS_DEFAULT / GAIT_TICK_MS);
    ck(strcmp(motion_trans_name(), "就绪") == 0, "淡入完成 → 就绪");
}

/* T6：跑换走也要收脚 → 停留 → 淡入 */
static void t6_walk_to_walk(void)
{
    printf("\nT6 跑 → 走：收脚、停留、再淡入\n");
    reset_fw();
    motion_walk(GAIT_PATTERN_TROT, 100);
    beats(GAIT_FADE_MS_DEFAULT / GAIT_TICK_MS + 2);

    motion_walk(GAIT_PATTERN_WALK, 100);
    ck(strcmp(motion_trans_name(), "回站姿中") == 0, "跑→走先淡出收脚");
    beats(GAIT_FADE_MS_DEFAULT / GAIT_TICK_MS);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "收脚后进入站姿停留");

    beats(GAIT_SETTLE_MS_DEFAULT / GAIT_TICK_MS + 2);
    ck(strcmp(motion_trans_name(), "起步淡入中") == 0 ||
       strcmp(motion_trans_name(), "就绪") == 0, "停留结束后才淡入新步态");
    ck(motion_params()->pattern == GAIT_PATTERN_WALK, "步态已换成 walk");
}

/* T7：时间到了但输出还没追平 → 必须继续等，不能只看计时 */
static void t7_waits_for_output(void)
{
    printf("\nT7 舵机滞后时，停留会等输出收敛（不只是等时间）\n");
    reset_fw();
    motion_set_settle_trans(GAIT_SETTLE_MS_MIN);      /* 100ms = 5 拍 */

    motion_pose(POSE_LIE);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS + 2);
    motion_walk(GAIT_PATTERN_WALK, 100);
    beats(POSE_TRANS_MS_DEFAULT / GAIT_TICK_MS);
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0, "已进入停留");

    /* 模拟舵机被拖慢：进入停留后实际输出还差 60°，rate_limit 每拍只能追 11°，
     * 需要约 6 拍才收敛，明显超过 100ms(5 拍) 的停留时间。 */
    for (int j = 0; j < J_COUNT; j++) s_out[j] += 60.0f;
    printf("     注入滞后后输出误差 %.2f°\n", output_stand_error());

    const int time_beats = GAIT_SETTLE_MS_MIN / GAIT_TICK_MS;   /* 5 拍 */
    beats(time_beats);
    printf("     第 %d 拍（时间已够）→ %s / 输出误差 %.2f°\n",
           time_beats, motion_trans_name(), output_stand_error());
    ck(strcmp(motion_trans_name(), "站姿停留中") == 0,
       "停留时间已到，但输出没收敛 → 仍不放行");
    ck(motion_mode() == MODE_POSE, "目标动作仍被挂起");

    beats(6);
    ck(motion_mode() == MODE_WALK, "输出收敛并连续稳定后才放行");
}

int main(void)
{
    printf("Spider8 站姿停留状态机 PC 回归单测（真实 motion.c）\n");
    printf("拍长 %dms / 姿态 %dms / 淡入淡出 %dms / 停留 %dms + guard %dms / 收敛 %.1f°×%d 拍\n",
           GAIT_TICK_MS, POSE_TRANS_MS_DEFAULT, GAIT_FADE_MS_DEFAULT,
           GAIT_SETTLE_MS_DEFAULT, GAIT_SETTLE_GUARD_MS,
           GAIT_SETTLE_TOL_DEG, GAIT_SETTLE_OK_BEATS);

    t1_timeline();
    t2_change_target();
    t3_guard();
    t4_standhold();
    t5_direct();
    t6_walk_to_walk();
    t7_waits_for_output();

    printf("\n%s（%d 项失败）\n\n", g_fail ? "存在失败项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
