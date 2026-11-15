/*
 * Spider8 步态引擎实现
 *
 * 核心思路（和 OpenCat 的区别就在这里）：
 *   OpenCat：相位 → 余弦波 → 肩舵机角度，膝盖全程锁死，靠"斜腿 + 摩擦"蹭。
 *   我们   ：相位 → 脚的【目标位置】(y, z) → 逆运动学 → 髋/膝两个角。
 *
 * 为什么要多这一步：2-DOF 腿一摆髋，脚就画圆弧上下起伏。膝角锁死时
 * 支撑相脚会在 0~6.7mm 之间漂，机身跟着颠。IK 让膝盖反向补偿，
 * 把脚按在地面上，机身起伏才能做到 0.000mm。
 */
#include <math.h>
#include <string.h>

#include "gait.h"

#define SPI_PI      3.14159265358979f
#define DEG2RAD     (SPI_PI / 180.0f)
#define RAD2DEG     (180.0f / SPI_PI)

/* 相位表（0~255，256 = 一个完整周期）
 * 顺序与 LEG_FL/FR/RL/RR 对应。波步从【后腿先动】传波：
 *   RL 0 → FL 64 → RR 128 → FR 192
 * 真实蜘蛛/狗都是后腿先蹬，把重心往前送，前腿才有空间迈；
 * 侧摆比"前腿先动"小 30%。 */
static const uint8_t PATTERN_PHASE[GAIT_PATTERN_COUNT][LEG_COUNT] = {
    /* WALK  */ {  64, 192,   0, 128 },
    /* TROT  */ {   0, 128, 128,   0 },
    /* PACE  */ {   0, 128,   0, 128 },
    /* BOUND */ {   0,   0, 128, 128 },
};

static const char *PATTERN_NAME[GAIT_PATTERN_COUNT] = {
    "walk", "trot", "pace", "bound"
};

const char *gait_pattern_name(uint8_t pattern)
{
    if (pattern >= GAIT_PATTERN_COUNT) {
        return "?";
    }
    return PATTERN_NAME[pattern];
}

/* ========================================================================
 * 参数可行性估算
 *
 * 峰值角速度来自摆动相：抬脚包络 sin² 在中段变化最快，而"抬 1mm 需要
 * 膝转多少度"由几何决定 —— 机身越高、腿越直，这个灵敏度越大。
 * 所以同样抬 16mm，蹲着走和站着走的角速度需求是不一样的。
 * ====================================================================== */
void gait_estimate(const gait_params_t *p, float *max_rate_dps, float *mm_per_cycle)
{
    uint32_t tpc = (uint32_t)p->period_ms / GAIT_TICK_MS;
    if (tpc < 2) {
        tpc = 2;
    }

    float prev_hip[LEG_COUNT] = { 0 };
    float prev_knee[LEG_COUNT] = { 0 };
    float rate = 0.0f;

    for (uint32_t t = 0; t < tpc; t++) {
        gait_out_t o;
        gait_step(p, t, tpc, &o);

        if (t > 0) {
            for (int l = 0; l < LEG_COUNT; l++) {
                const float rh = fabsf(o.hip_deg[l]  - prev_hip[l])  / (float)GAIT_TICK_MS * 1000.0f;
                const float rk = fabsf(o.knee_deg[l] - prev_knee[l]) / (float)GAIT_TICK_MS * 1000.0f;
                if (rh > rate) rate = rh;
                if (rk > rate) rate = rk;
            }
        }
        for (int l = 0; l < LEG_COUNT; l++) {
            prev_hip[l]  = o.hip_deg[l];
            prev_knee[l] = o.knee_deg[l];
        }
    }

    if (max_rate_dps) {
        *max_rate_dps = rate;
    }
    if (mm_per_cycle) {
        /* 支撑相脚相对机身从 +A 匀速移到 -A = 机身前进 2A */
        *mm_per_cycle = 2.0f * p->stride_mm * ((float)p->speed_pct / 100.0f);
    }
}

/* ========================================================================
 * 屈膝角 → 髋角
 *
 *   y = L1·sin(θ1) + L2·sin(θ1+θ2) = 0
 *   → tan(θ1) = -L2·sin(θ2) / (L1 + L2·cos(θ2))
 *
 * 为什么必须联立：如果髋角可以随便给，四条腿的脚会整体前后偏移几毫米，
 * 重心就跑出支撑三角形 —— 站着都在往前栽。
 * ====================================================================== */
float gait_hip_for_knee(float knee_deg)
{
    const float t2 = knee_deg * DEG2RAD;
    return atan2f(-GAIT_SHIN_MM * sinf(t2),
                  GAIT_THIGH_MM + GAIT_SHIN_MM * cosf(t2)) * RAD2DEG;
}

float gait_height_for_knee(float knee_deg)
{
    const float t2 = knee_deg * DEG2RAD;
    const float t1 = atan2f(-GAIT_SHIN_MM * sinf(t2),
                            GAIT_THIGH_MM + GAIT_SHIN_MM * cosf(t2));
    return GAIT_THIGH_MM * cosf(t1) + GAIT_SHIN_MM * cosf(t1 + t2);
}

/* ========================================================================
 * 两连杆逆运动学（余弦定理）
 *
 * 膝角强制取负 —— 统一向后折，四条腿同构，不会左右腿折法不一样。
 * d 超出 [|L1-L2|, L1+L2] 时夹住并置 clamped，调用方据此判断"够不着"。
 * ====================================================================== */
void gait_ik2(float y_mm, float zdown_mm,
              float *hip_deg, float *knee_deg, int8_t *clamped)
{
    const float l1 = GAIT_THIGH_MM;
    const float l2 = GAIT_SHIN_MM;
    const float d_max = (l1 + l2) * 0.998f;     /* 留一点余量，别解到完全伸直 */
    const float d_min = fabsf(l1 - l2) + 0.5f;

    float d = sqrtf(y_mm * y_mm + zdown_mm * zdown_mm);
    int8_t c = 0;

    if (d > d_max)      { d = d_max; c =  1; }
    else if (d < d_min) { d = d_min; c = -1; }

    float cos_k = (d * d - l1 * l1 - l2 * l2) / (2.0f * l1 * l2);
    if (cos_k >  1.0f) cos_k =  1.0f;
    if (cos_k < -1.0f) cos_k = -1.0f;

    const float knee = -acosf(cos_k);
    const float hip  = atan2f(y_mm, zdown_mm) -
                       atan2f(l2 * sinf(knee), l1 + l2 * cosf(knee));

    if (hip_deg)  *hip_deg  = hip * RAD2DEG;
    if (knee_deg) *knee_deg = knee * RAD2DEG;
    if (clamped)  *clamped  = c;
}

/* ========================================================================
 * 静态位姿解算
 * ====================================================================== */
void gait_solve_static(float knee_deg,
                       const float foot_y[LEG_COUNT],
                       const float foot_lift[LEG_COUNT],
                       float hip_out[LEG_COUNT],
                       float knee_out[LEG_COUNT])
{
    const float hip_h = gait_height_for_knee(knee_deg);

    for (int l = 0; l < LEG_COUNT; l++) {
        const float y    = foot_y    ? foot_y[l]    : 0.0f;
        const float lift = foot_lift ? foot_lift[l] : 0.0f;

        float hip, knee;
        int8_t clamped;
        gait_ik2(y, hip_h - lift, &hip, &knee, &clamped);

        hip_out[l]  = hip;
        knee_out[l] = knee;
    }
}

/* ========================================================================
 * 步态解算
 * ====================================================================== */
void gait_apply_fade(const gait_params_t *src, float fade, gait_params_t *dst)
{
    if (!src || !dst) {
        return;
    }
    *dst = *src;

    if (fade < 0.0f) fade = 0.0f;
    if (fade > 1.0f) fade = 1.0f;
    /* fade=1 必须与原参数逐位相同：已验收的 walk 100 不能被取整改动 */
    if (fade >= 1.0f) {
        return;
    }

    dst->speed_pct = (int8_t)(src->speed_pct * fade + 0.5f);
    const float t  = src->turn_pct * fade;
    dst->turn_pct  = (int8_t)((t >= 0.0f) ? (t + 0.5f) : (t - 0.5f));
}

void gait_step(const gait_params_t *p, uint32_t tick,
               uint32_t ticks_per_cycle, gait_out_t *out)
{
    if (ticks_per_cycle < 2) {
        ticks_per_cycle = 2;
    }

    /* 占空比裁剪：0% 会让摆动窗口除零，>45% 波步就不是波步了 */
    uint32_t duty = p->duty_pct;
    if (duty < 10) duty = 10;
    if (duty > 45) duty = 45;

    const uint8_t  base      = (uint8_t)((tick * 256u) / ticks_per_cycle);
    const uint8_t  swing_end = (uint8_t)((256u * duty) / 100u);
    const uint8_t *offset    = PATTERN_PHASE[(p->pattern < GAIT_PATTERN_COUNT)
                                             ? p->pattern : GAIT_PATTERN_WALK];

    const float hip_h  = gait_height_for_knee(p->stand_knee_deg);
    const float speed_k = (float)p->speed_pct / 100.0f;
    const float turn_k  = (float)p->turn_pct / 100.0f;

    out->phase = base;

    for (int l = 0; l < LEG_COUNT; l++) {
        uint8_t ph = (uint8_t)(base + offset[l]);

        const bool swinging = (ph < swing_end);
        float u;

        if (swinging) {
            u = (float)ph / (float)swing_end;
        } else {
            u = (float)(ph - swing_end) / (float)(256u - swing_end);
        }

        /* 转向与前进【解耦】：左侧步幅放大、右侧缩小 → 机身右转。
         * 旧公式 stride = stride_mm × speed_k × (1 ± turn_k×0.6)，
         * speed=0 时整条被乘成 0，「原地左转/右转」完全失效。
         * 新公式把前进分量与自转分量相加；speed=100 时与旧版逐位相同。 */
        const bool is_left = (l == LEG_FL || l == LEG_RL);
        const float fwd    = p->stride_mm * speed_k;
        const float spin   = p->stride_mm * turn_k * GAIT_TURN_GAIN;
        const float stride = fwd + (is_left ? spin : -spin);

        /* 抬脚按「这一步实际迈多远」缩放：步幅 0 → 抬脚 0。
         * 旧版 lift 恒为 lift_mm，speed=20% 时前后只走 4.4mm 却抬 14mm，
         * 看着就是原地高抬腿；speed=0 时更是原地空蹬。
         * stride_mm 恒 > 0（fade 不缩放它），夹到 1.0 防止转向叠加时抬过头。 */
        float ref = fabsf(p->stride_mm);
        if (ref < 0.001f) ref = 0.001f;
        float lift_scale = fabsf(stride) / ref;
        if (lift_scale > 1.0f) lift_scale = 1.0f;

        float y, lift;
        if (swinging) {
            /* 余弦缓动：起落瞬间速度为 0，不砸地 */
            const float s = (1.0f - cosf(SPI_PI * u)) * 0.5f;
            y = -stride + 2.0f * stride * s;
            /* sin² 包络：抬脚同样两端平滑 */
            const float e = sinf(SPI_PI * u);
            lift = p->lift_mm * e * e * lift_scale;
        } else {
            /* 线性后移 = 相对地面匀速 → 机身匀速前进 */
            y = stride - 2.0f * stride * u;
            lift = 0.0f;
        }

        float hip, knee;
        int8_t clamped;
        gait_ik2(y, hip_h - lift, &hip, &knee, &clamped);

        out->hip_deg[l]  = hip;
        out->knee_deg[l] = knee;
        out->swinging[l] = swinging;
    }
}
