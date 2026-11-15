/*
 * gait.c 的 PC 端单元测试
 *
 * 为什么要在 PC 上测：运动学是纯数学，和硬件无关。把它先在这里跑对，
 * 板子上就只剩"接线对不对"一个问题，排查范围直接砍一半。
 *
 * 编译（Git Bash）：
 *   gcc -std=c11 -O2 -I firmware/main tools/fw_test/test_gait.c \
 *       firmware/main/gait.c -lm -o /tmp/test_gait && /tmp/test_gait
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "gait.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;

static void check(const char *what, int ok, const char *detail)
{
    printf("  %s %-46s %s\n", ok ? "✓" : "✗", what, detail ? detail : "");
    if (!ok) g_fail++;
}

/* 正运动学：关节角 → 脚相对髋点的位置，用来验证 IK 反解对不对 */
static void fk(float hip_deg, float knee_deg, float *y, float *zdown)
{
    const float t1 = hip_deg * (float)M_PI / 180.0f;
    const float t2 = knee_deg * (float)M_PI / 180.0f;
    *y     = GAIT_THIGH_MM * sinf(t1) + GAIT_SHIN_MM * sinf(t1 + t2);
    *zdown = GAIT_THIGH_MM * cosf(t1) + GAIT_SHIN_MM * cosf(t1 + t2);
}

int main(void)
{
    char buf[160];

    /* ---------------------------------------------------- Q1 站立姿态 */
    printf("\n=== Q1 站立姿态自洽 ===\n");
    {
        const float hip = gait_hip_for_knee(GAIT_STAND_KNEE_DEG);
        const float h   = gait_height_for_knee(GAIT_STAND_KNEE_DEG);

        snprintf(buf, sizeof(buf), "髋 %.2f°  髋轴离地 %.2fmm", hip, h);
        check("屈膝 -38° 反解髋角", fabsf(hip - 19.0f) < 0.6f, buf);

        /* 两腿等长 → 髋角应精确等于 -膝角/2 */
        snprintf(buf, sizeof(buf), "%.4f vs %.4f", hip, -GAIT_STAND_KNEE_DEG / 2.0f);
        check("等长腿时 髋角 = -膝角/2", fabsf(hip + GAIT_STAND_KNEE_DEG / 2.0f) < 0.01f, buf);

        float y, z;
        fk(hip, GAIT_STAND_KNEE_DEG, &y, &z);
        snprintf(buf, sizeof(buf), "脚前向偏移 %.4fmm", y);
        check("脚落在髋点正下方", fabsf(y) < 0.01f, buf);

        snprintf(buf, sizeof(buf), "%.2fmm（全伸 110mm）", z);
        check("站立高度在合理区间 90~110mm", z > 90.0f && z < 110.0f, buf);
    }

    /* ---------------------------------------------------- Q2 IK 往返 */
    printf("\n=== Q2 逆运动学往返精度（IK → FK）===\n");
    {
        const float h = gait_height_for_knee(GAIT_DEFAULT_STAND_KNEE_DEG);
        float worst = 0.0f;
        int clamped_n = 0;

        for (int i = -20; i <= 20; i++) {
            const float y = (float)i;                 /* 脚前后 -20..20mm */
            for (int k = 0; k <= 16; k++) {
                const float lift = (float)k;          /* 抬脚 0..16mm */
                float hip, knee;
                int8_t c;
                gait_ik2(y, h - lift, &hip, &knee, &c);
                if (c) { clamped_n++; continue; }

                float fy, fz;
                fk(hip, knee, &fy, &fz);
                const float err = sqrtf((fy - y) * (fy - y) +
                                        (fz - (h - lift)) * (fz - (h - lift)));
                if (err > worst) worst = err;
            }
        }
        snprintf(buf, sizeof(buf), "最大误差 %.6fmm，不可达 %d 组", worst, clamped_n);
        check("858 组 (y, lift) 全部反解回原位置", worst < 0.01f, buf);
    }

    /* ---------------------------------------------------- Q3 步态稳定性 */
    printf("\n=== Q3 各步态的最少支撑腿数 ===\n");
    {
        static const struct { const char *name; uint8_t pat; int expect; } cases[] = {
            { "walk  波步", GAIT_PATTERN_WALK,  3 },
            { "trot  小跑", GAIT_PATTERN_TROT,  2 },
            { "pace  溜蹄", GAIT_PATTERN_PACE,  2 },
            { "bound 跳跃", GAIT_PATTERN_BOUND, 2 },
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            gait_params_t p = {
                .period_ms = 1200, .duty_pct = 25, .stride_mm = 22.0f,
                .lift_mm = 16.0f, .stand_knee_deg = GAIT_DEFAULT_STAND_KNEE_DEG,
                .speed_pct = 100, .turn_pct = 0, .pattern = cases[i].pat,
            };

            const uint32_t tpc = p.period_ms / GAIT_TICK_MS;
            int min_sup = 4;

            for (uint32_t t = 0; t < tpc; t++) {
                gait_out_t o;
                gait_step(&p, t, tpc, &o);
                int sup = 0;
                for (int l = 0; l < LEG_COUNT; l++) {
                    if (!o.swinging[l]) sup++;
                }
                if (sup < min_sup) min_sup = sup;
            }

            snprintf(buf, sizeof(buf), "实测 %d 条（期望 %d）", min_sup, cases[i].expect);
            check(cases[i].name, min_sup == cases[i].expect, buf);
        }
    }

    /* ---------------------------------------------------- Q4 舵机行程 */
    printf("\n=== Q4 全周期关节角范围 vs MG90S 行程 ===\n");
    {
        gait_params_t p = {
            .period_ms = 1200, .duty_pct = 25, .stride_mm = 22.0f,
            .lift_mm = 16.0f, .stand_knee_deg = GAIT_STAND_KNEE_DEG,
            .speed_pct = 100, .turn_pct = 0, .pattern = GAIT_PATTERN_WALK,
        };
        const uint32_t tpc = p.period_ms / GAIT_TICK_MS;

        float hip_lo = 1e9f, hip_hi = -1e9f, kn_lo = 1e9f, kn_hi = -1e9f;
        float prev_hip[LEG_COUNT], prev_kn[LEG_COUNT];
        float max_rate = 0;
        int first = 1;

        for (uint32_t t = 0; t < tpc; t++) {
            gait_out_t o;
            gait_step(&p, t, tpc, &o);
            for (int l = 0; l < LEG_COUNT; l++) {
                if (o.hip_deg[l] < hip_lo) hip_lo = o.hip_deg[l];
                if (o.hip_deg[l] > hip_hi) hip_hi = o.hip_deg[l];
                if (o.knee_deg[l] < kn_lo) kn_lo = o.knee_deg[l];
                if (o.knee_deg[l] > kn_hi) kn_hi = o.knee_deg[l];
                if (!first) {
                    float rh = fabsf(o.hip_deg[l] - prev_hip[l]) / GAIT_TICK_MS * 1000.0f;
                    float rk = fabsf(o.knee_deg[l] - prev_kn[l]) / GAIT_TICK_MS * 1000.0f;
                    if (rh > max_rate) max_rate = rh;
                    if (rk > max_rate) max_rate = rk;
                }
                prev_hip[l] = o.hip_deg[l];
                prev_kn[l]  = o.knee_deg[l];
            }
            first = 0;
        }

        /* 关节角 → 舵机角（站立为 90°） */
        const float ref_hip = gait_hip_for_knee(GAIT_STAND_KNEE_DEG);
        const float ref_kn  = GAIT_STAND_KNEE_DEG;
        const float s_hip_lo = 90.0f + (hip_lo - ref_hip);
        const float s_hip_hi = 90.0f + (hip_hi - ref_hip);
        const float s_kn_lo  = 90.0f + (kn_lo  - ref_kn);
        const float s_kn_hi  = 90.0f + (kn_hi  - ref_kn);

        snprintf(buf, sizeof(buf), "髋舵机 %.1f~%.1f°", s_hip_lo, s_hip_hi);
        check("髋舵机行程 0~180°", s_hip_lo > 0 && s_hip_hi < 180, buf);

        snprintf(buf, sizeof(buf), "膝舵机 %.1f~%.1f°", s_kn_lo, s_kn_hi);
        check("膝舵机行程 0~180°", s_kn_lo > 0 && s_kn_hi < 180, buf);

        snprintf(buf, sizeof(buf), "%.0f°/s（MG90S 上限 600°/s）", max_rate);
        check("峰值角速度在舵机能力内", max_rate < 600.0f, buf);
    }

    /* ---------------------------------------------------- Q5 蹲下/趴下 */
    printf("\n=== Q5 蹲下 / 趴下高度与舵机角 ===\n");
    {
        const float ref_hip = gait_hip_for_knee(GAIT_STAND_KNEE_DEG);
        static const struct { const char *n; float knee; float h_min; float h_max; } cases[] = {
            { "蹲下", GAIT_DEFAULT_STAND_KNEE_DEG + POSE_CROUCH_DK, 85.0f, 100.0f },
            { "趴下", GAIT_DEFAULT_STAND_KNEE_DEG + POSE_LIE_DK,    70.0f,  80.0f },
        };

        for (size_t i = 0; i < 2; i++) {
            const float hip = gait_hip_for_knee(cases[i].knee);
            const float h   = gait_height_for_knee(cases[i].knee);
            const float s_hip = 90.0f + (hip - ref_hip);
            const float s_kn  = 90.0f + (cases[i].knee - GAIT_STAND_KNEE_DEG);

            snprintf(buf, sizeof(buf), "离地 %.1fmm 舵机 %.1f/%.1f°", h, s_hip, s_kn);
            check(cases[i].n, h > cases[i].h_min && h < cases[i].h_max &&
                              s_hip > 0 && s_hip < 180 && s_kn > 0 && s_kn < 180, buf);
        }
    }

    /* ---------------------------------------------------- Q6 转向差速 */
    printf("\n=== Q6 差速转向 ===\n");
    {
        gait_params_t p = {
            .period_ms = 1200, .duty_pct = 25, .stride_mm = 22.0f,
            .lift_mm = 16.0f, .stand_knee_deg = GAIT_STAND_KNEE_DEG,
            .speed_pct = 100, .turn_pct = 0, .pattern = GAIT_PATTERN_WALK,
        };
        const uint32_t tpc = p.period_ms / GAIT_TICK_MS;

        /* 波步里四条腿相位不同，不能直接比同一时刻的角度。
         * 正确做法：比"一个周期内每条腿髋角的摆幅"。
         * turn=0 时四条腿摆幅必须完全一样（否则就是在自己拐弯）。 */
        float amp[LEG_COUNT];
        for (int pass = 0; pass < 2; pass++) {
            p.turn_pct = (pass == 0) ? 0 : 100;
            float lo[LEG_COUNT], hi[LEG_COUNT];
            for (int l = 0; l < LEG_COUNT; l++) { lo[l] = 1e9f; hi[l] = -1e9f; }

            for (uint32_t t = 0; t < tpc; t++) {
                gait_out_t o;
                gait_step(&p, t, tpc, &o);
                for (int l = 0; l < LEG_COUNT; l++) {
                    if (o.hip_deg[l] < lo[l]) lo[l] = o.hip_deg[l];
                    if (o.hip_deg[l] > hi[l]) hi[l] = o.hip_deg[l];
                }
            }
            for (int l = 0; l < LEG_COUNT; l++) {
                amp[l] = hi[l] - lo[l];
            }

            if (pass == 0) {
                const float d = fmaxf(fmaxf(fabsf(amp[0] - amp[1]), fabsf(amp[2] - amp[3])),
                                      fmaxf(fabsf(amp[0] - amp[2]), fabsf(amp[1] - amp[3])));
                snprintf(buf, sizeof(buf), "FL%.1f FR%.1f RL%.1f RR%.1f", amp[0], amp[1], amp[2], amp[3]);
                check("turn=0 时四腿摆幅一致", d < 0.5f, buf);
            } else {
                snprintf(buf, sizeof(buf), "左 %.1f/%.1f° 右 %.1f/%.1f°",
                         amp[0], amp[2], amp[1], amp[3]);
                check("turn=100 左侧摆幅 > 右侧（右转）",
                      amp[0] > amp[1] + 1.0f && amp[2] > amp[3] + 1.0f, buf);
            }
        }
    }

    /* ------------------------- Q7 站姿多折 10°（2026-11-13 新增） */
    printf("\n=== Q7 新默认站姿 −48°（装配基准 −38° 不动）===\n");
    {
        const float kn      = GAIT_DEFAULT_STAND_KNEE_DEG;
        const float hip     = gait_hip_for_knee(kn);
        const float h       = gait_height_for_knee(kn);
        const float ref_hip = gait_hip_for_knee(GAIT_STAND_KNEE_DEG);
        const float ref_h   = gait_height_for_knee(GAIT_STAND_KNEE_DEG);

        snprintf(buf, sizeof(buf), "膝 %.1f° 髋 %.1f° 高度 %.1fmm", kn, hip, h);
        check("膝多折 10° → 髋只多摆 5°",
              fabsf(hip - 24.0f) < 0.1f &&
              fabsf((hip - ref_hip) - 5.0f) < 0.1f, buf);

        float y, z;
        fk(hip, kn, &y, &z);
        snprintf(buf, sizeof(buf), "脚前向偏移 %.4fmm", y);
        check("新站姿脚仍在髋点正下方", fabsf(y) < 0.01f, buf);

        snprintf(buf, sizeof(buf), "降 %.2fmm（原 %.2fmm → 现 %.2fmm）", ref_h - h, ref_h, h);
        check("机身确实降低 3~4mm", ref_h - h > 3.0f && ref_h - h < 4.0f, buf);

        /* 装配基准不动 ⇒ 舵机相对 90° 只走 −10°/+5°，8 个 trim 原封不动 */
        const float s_hip = 90.0f + (hip - ref_hip);
        const float s_kn  = 90.0f + (kn - GAIT_STAND_KNEE_DEG);
        snprintf(buf, sizeof(buf), "髋 %.1f° 膝 %.1f°（不含 trim）", s_hip, s_kn);
        check("站姿舵机角仍在 0~180° 内",
              s_hip > 0 && s_hip < 180 && s_kn > 0 && s_kn < 180, buf);

        gait_params_t p = {
            .period_ms = 1200, .duty_pct = 25, .stride_mm = 22.0f,
            .lift_mm = 14.0f, .stand_knee_deg = kn,
            .speed_pct = 100, .turn_pct = 0, .pattern = GAIT_PATTERN_WALK,
        };
        float rate_new = 0, rate_old = 0, dummy = 0;
        gait_estimate(&p, &rate_new, &dummy);
        p.stand_knee_deg = GAIT_STAND_KNEE_DEG;
        gait_estimate(&p, &rate_old, &dummy);
        snprintf(buf, sizeof(buf), "新 %.0f°/s vs 原 %.0f°/s", rate_new, rate_old);
        check("峰值角速度不超 MG90S 600°/s", rate_new < 600.0f, buf);

        const float crouch = kn + POSE_CROUCH_DK;
        const float lie    = kn + POSE_LIE_DK;
        snprintf(buf, sizeof(buf), "蹲 %.0f° 趴 %.0f°", crouch, lie);
        check("蹲/趴膝角在 −100° 软限位内",
              crouch > -100.0f && lie > -100.0f && lie < crouch, buf);
    }

    printf("\n%s（%d 项失败）\n\n", g_fail ? "存在失败项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
