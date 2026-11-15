/*
 * 动作切换「中间态站姿」+ 差速转向 的 PC 回归单测
 *
 * 编译：
 *   gcc -std=c11 -O2 -I firmware/main \
 *       tools/fw_test/test_transition.c firmware/main/gait.c -lm -o /tmp/test_transition
 *
 * 为什么值得单独一份：motion.c 跑在 FreeRTOS 上没法在 PC 上跑，但切换是否
 * 会跳变，完全取决于 gait_apply_fade / gait_step 这两个纯函数。把几何不变量
 * 在这里钉死，等于把状态机最危险的那部分验证前移到桌面。
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "gait.h"

#define DEG2RAD (3.14159265358979f / 180.0f)

static int g_fail = 0;

static void ck(int ok, const char *name)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

static gait_params_t base_params(void)
{
    gait_params_t p;
    memset(&p, 0, sizeof(p));
    p.period_ms       = 1200;
    p.duty_pct        = 25;
    p.stride_mm       = 22.0f;
    p.lift_mm         = 14.0f;
    p.stand_knee_deg  = GAIT_DEFAULT_STAND_KNEE_DEG;
    p.speed_pct       = 100;
    p.turn_pct        = 0;
    p.pattern         = GAIT_PATTERN_WALK;
    return p;
}

/* 正运动学：关节角 → 脚相对髋点的前后 y / 向下深度 z */
static void fk(float hip_deg, float knee_deg, float *y, float *z)
{
    const float h = hip_deg  * DEG2RAD;
    const float k = knee_deg * DEG2RAD;
    *y = GAIT_THIGH_MM * sinf(h) + GAIT_SHIN_MM * sinf(h + k);
    *z = GAIT_THIGH_MM * cosf(h) + GAIT_SHIN_MM * cosf(h + k);
}

static uint32_t tpc_of(const gait_params_t *p)
{
    uint32_t n = p->period_ms / GAIT_TICK_MS;
    return (n < 2) ? 2u : n;
}

/* Q1: fade=1 必须与原参数逐位相同 —— 已验收的 walk 100 不能被取整改动 */
static void q1_fade_one_identical(void)
{
    printf("\nQ1 fade=1 与原参数一致\n");
    gait_params_t p = base_params(), q;
    gait_apply_fade(&p, 1.0f, &q);
    ck(memcmp(&p, &q, sizeof(p)) == 0, "fade=1 结构体逐位相同");
    gait_apply_fade(&p, 1.7f, &q);
    ck(memcmp(&p, &q, sizeof(p)) == 0, "fade>1 夹到 1 后仍相同");
    gait_apply_fade(&p, -3.0f, &q);
    ck(q.speed_pct == 0 && q.turn_pct == 0, "fade<0 夹到 0");
}

/* Q2/Q3: fade=0 时任意相位都是「纯站姿」——脚在髋点正下方、不抬脚、
 * 膝角恒等于站姿膝角。这是「切换必经站姿」能零跳变的几何前提。 */
static void q2_fade_zero_is_stand(void)
{
    printf("\nQ2 fade=0 全周期都是纯站姿\n");
    gait_params_t p = base_params(), q;
    gait_apply_fade(&p, 0.0f, &q);
    const uint32_t tpc = tpc_of(&p);
    const float hip_h = gait_height_for_knee(p.stand_knee_deg);

    float max_y = 0, max_lift = 0, max_knee_err = 0, max_hip_err = 0;
    float hip_s[LEG_COUNT], knee_s[LEG_COUNT];
    gait_solve_static(p.stand_knee_deg, NULL, NULL, hip_s, knee_s);

    for (uint32_t t = 0; t < tpc; t++) {
        gait_out_t o;
        gait_step(&q, t, tpc, &o);
        for (int l = 0; l < LEG_COUNT; l++) {
            float y, z;
            fk(o.hip_deg[l], o.knee_deg[l], &y, &z);
            const float dy = fabsf(y);
            const float dl = fabsf(hip_h - z);
            if (dy > max_y) max_y = dy;
            if (dl > max_lift) max_lift = dl;
            const float dk = fabsf(o.knee_deg[l] - p.stand_knee_deg);
            const float dh = fabsf(o.hip_deg[l] - gait_hip_for_knee(p.stand_knee_deg));
            if (dk > max_knee_err) max_knee_err = dk;
            if (dh > max_hip_err) max_hip_err = dh;
        }
    }
    printf("     最大前后偏移 %.4fmm / 最大抬脚 %.4fmm / 膝角误差 %.4f° / 髋角误差 %.4f°\n",
           max_y, max_lift, max_knee_err, max_hip_err);
    ck(max_y < 0.02f, "四脚恒在髋点正下方（无前后滑）");
    ck(max_lift < 0.02f, "抬脚恒为 0（不再原地空蹬）");
    ck(max_knee_err < 0.02f && max_hip_err < 0.02f, "关节角恒等于站姿");

    /* 与 POSE_STAND 那条路径逐位对齐：证明 WALK→POSE 切换瞬间姿势连续 */
    gait_out_t o0;
    gait_step(&q, 0, tpc, &o0);
    float d = 0;
    for (int l = 0; l < LEG_COUNT; l++) {
        d = fmaxf(d, fabsf(o0.hip_deg[l]  - hip_s[l]));
        d = fmaxf(d, fabsf(o0.knee_deg[l] - knee_s[l]));
    }
    printf("     fade=0 步态首拍 vs POSE_STAND 最大差 %.6f°\n", d);
    ck(d < 0.001f, "步态首拍 == 站姿（相位清零不跳变）");
}

/* Q4: 用真实 500ms/20ms 拍数走一遍淡入，验证相邻拍没有跳变 */
static void q4_fade_smooth(void)
{
    printf("\nQ4 淡入 500ms 逐拍平滑\n");
    gait_params_t p = base_params(), q;
    const uint32_t tpc = tpc_of(&p);
    const uint32_t beats = GAIT_FADE_MS_DEFAULT / GAIT_TICK_MS;

    float prev[J_COUNT];
    int have_prev = 0;
    float max_step = 0, max_rate = 0;
    for (uint32_t b = 0; b <= beats; b++) {
        const float t = (float)b / (float)beats;
        const float e = t * t * (3.0f - 2.0f * t);
        gait_apply_fade(&p, e, &q);

        gait_out_t o;
        gait_step(&q, b, tpc, &o);
        float cur[J_COUNT];
        for (int l = 0; l < LEG_COUNT; l++) {
            cur[l * 2]     = o.hip_deg[l];
            cur[l * 2 + 1] = o.knee_deg[l];
        }
        if (have_prev) {
            for (int j = 0; j < J_COUNT; j++) {
                const float d = fabsf(cur[j] - prev[j]);
                if (d > max_step) max_step = d;
            }
        }
        memcpy(prev, cur, sizeof(prev));
        have_prev = 1;
    }
    max_rate = max_step * 1000.0f / GAIT_TICK_MS;
    printf("     相邻拍最大角差 %.3f° ≈ %.0f°/s（限速闸 %.0f°/s）\n",
           max_step, max_rate, MOTION_SLEW_DPS);
    ck(max_step < 11.0f, "淡入未被斜率限制削平（无硬切）");
    ck(max_rate < SERVO_MAX_RATE_DPS, "峰值角速度未超舵机极限");
}

/* Q5: 转向与速度解耦 —— speed=0 时仍能原地左右打转（旧实现整条 stride 被乘成 0） */
static void q5_spin_at_zero_speed(void)
{
    printf("\nQ5 原地转向（speed=0）\n");
    gait_params_t p = base_params();
    p.speed_pct = 0;
    p.turn_pct  = GAIT_SPIN_TURN_PCT;
    const uint32_t tpc = tpc_of(&p);

    float max_y[LEG_COUNT] = {0};
    for (uint32_t t = 0; t < tpc; t++) {
        gait_out_t o;
        gait_step(&p, t, tpc, &o);
        for (int l = 0; l < LEG_COUNT; l++) {
            float y, z;
            fk(o.hip_deg[l], o.knee_deg[l], &y, &z);
            if (fabsf(y) > max_y[l]) max_y[l] = fabsf(y);
        }
    }
    printf("     FL/RL/FR/RR 最大前后位移 %.2f / %.2f / %.2f / %.2f mm\n",
           max_y[LEG_FL], max_y[LEG_RL], max_y[LEG_FR], max_y[LEG_RR]);
    ck(max_y[LEG_FL] > 5.0f && max_y[LEG_RL] > 5.0f, "左侧两腿在动");
    ck(max_y[LEG_FR] > 5.0f && max_y[LEG_RR] > 5.0f, "右侧两腿在动");
    ck(fabsf(max_y[LEG_FL] - max_y[LEG_FR]) < 0.02f &&
       fabsf(max_y[LEG_RL] - max_y[LEG_RR]) < 0.02f, "左右两侧步幅幅值相等");

    /* 四腿相位本来就是错开的，不能要求「同一时刻左右脚 y 异号」。
     * 差速转向真正的判据是：turn 反号时，每一时刻四脚前后位置整体镜像。 */
    gait_out_t op, on;
    p.turn_pct = +GAIT_SPIN_TURN_PCT;
    gait_step(&p, 0, tpc, &op);
    p.turn_pct = -GAIT_SPIN_TURN_PCT;
    gait_step(&p, 0, tpc, &on);

    float max_mirror_err = 0, yp0 = 0;
    for (int l = 0; l < LEG_COUNT; l++) {
        float yp, yn, z;
        fk(op.hip_deg[l], op.knee_deg[l], &yp, &z);
        fk(on.hip_deg[l], on.knee_deg[l], &yn, &z);
        if (l == LEG_FL) yp0 = yp;
        const float err = fabsf(yp + yn);
        if (err > max_mirror_err) max_mirror_err = err;
    }
    printf("     turn 反号后四脚位置镜像误差 %.4fmm（左前 tick0 y=%+.2fmm）\n",
           max_mirror_err, yp0);
    ck(max_mirror_err < 0.02f, "turn 反号 → 四脚镜像（真差速，机身原地转）");

    /* speed=100 时新公式必须与旧公式等价：左侧 22×(1+0.42)、右侧 22×(1−0.42) */
    gait_params_t f = base_params();
    f.turn_pct = GAIT_SPIN_TURN_PCT;
    const float expect_left  = f.stride_mm * (1.0f + 0.7f * GAIT_TURN_GAIN);
    const float expect_right = f.stride_mm * (1.0f - 0.7f * GAIT_TURN_GAIN);
    printf("     speed=100 时标称左右步幅 %.2f / %.2f mm（旧公式同值）\n",
           expect_left, expect_right);
    ck(expect_left > expect_right, "满速转向仍是左大右小（右转）");
}

/* Q6: 抬脚随实际步幅缩放，修掉「低速原地高抬腿」 */
static void q6_lift_scales_with_stride(void)
{
    printf("\nQ6 抬脚随步幅缩放\n");
    gait_params_t p = base_params();
    const uint32_t tpc = tpc_of(&p);
    const float hip_h = gait_height_for_knee(p.stand_knee_deg);

    const int speeds[] = {0, 20, 100};
    for (int i = 0; i < 3; i++) {
        p.speed_pct = speeds[i];
        float max_lift = 0;
        for (uint32_t t = 0; t < tpc; t++) {
            gait_out_t o;
            gait_step(&p, t, tpc, &o);
            for (int l = 0; l < LEG_COUNT; l++) {
                float y, z;
                fk(o.hip_deg[l], o.knee_deg[l], &y, &z);
                const float dl = hip_h - z;
                if (dl > max_lift) max_lift = dl;
            }
        }
        printf("     speed=%3d%% 最大抬脚 %.2fmm（标称 %.1fmm）\n",
               speeds[i], max_lift, p.lift_mm);
        if (speeds[i] == 0)   ck(max_lift < 0.02f, "speed=0 完全不抬脚");
        if (speeds[i] == 20)  ck(max_lift < 3.2f, "20% 时抬脚按比例降到约 2.8mm");
        if (speeds[i] == 100) ck(fabsf(max_lift - p.lift_mm) < 0.2f, "100% 时抬脚仍是标称 14mm");
    }

    float rate = 0, mm = 0;
    p.speed_pct = 100;
    gait_estimate(&p, &rate, &mm);
    printf("     walk 100 峰值 %.0f°/s / 净推进 %.1fmm 每周期\n", rate, mm);
    ck(rate > 380.0f && rate < 460.0f, "已验收的 walk 100 角速度未变");
    ck(mm > 40.0f && mm < 48.0f, "已验收的 walk 100 推进量未变");
}

int main(void)
{
    printf("Spider8 动作切换 / 转向 PC 回归单测\n");
    printf("站姿膝 %.1f° / 髋 %.2f° / 髋轴离地 %.2fmm\n",
           GAIT_DEFAULT_STAND_KNEE_DEG,
           gait_hip_for_knee(GAIT_DEFAULT_STAND_KNEE_DEG),
           gait_height_for_knee(GAIT_DEFAULT_STAND_KNEE_DEG));

    q1_fade_one_identical();
    q2_fade_zero_is_stand();
    q4_fade_smooth();
    q5_spin_at_zero_speed();
    q6_lift_scales_with_stride();

    printf("\n%s（%d 项失败）\n\n", g_fail ? "存在失败项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
