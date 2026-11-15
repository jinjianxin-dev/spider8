/*
 * 姿态过渡几何回归测试（PC 直编，不需要板子）
 *
 *   gcc -O2 -I firmware/main -o test_pose.exe test_pose.c ../firmware/main/gait.c -lm
 *
 * 验证 2026-11-11 修掉的那个 bug：站→趴时若把髋/膝各自等速插值，
 * 会破坏「髋 = −膝/2」，四只脚在地面前后滑 18mm（肉眼就是大腿猛地下去、
 * 发抖、小腿后动）。正确做法是只对膝角做 smoothstep 缓动、髋角每拍由 IK 反解。
 *
 * 断言：新路径脚前后位移恒 < 0.01mm、峰值膝角速度 < 120°/s、髋膝同时到位。
 * 同时打印旧路径的漂移量作为对照。退出码 0 = 通过。
 */
#include <stdio.h>
#include <math.h>
#include "gait.h"

#define RAD (3.14159265358979f / 180.0f)
#define TICK_MS       20
#define SLEW_DEG      (550.0f * TICK_MS / 1000.0f)   /* 旧实现：11°/拍 */
#define TRANS_MS      900.0f                          /* 新实现默认过渡时长 */

static void fk(float hip, float knee, float *y, float *z)
{
    float r1 = hip * RAD, r2 = (hip + knee) * RAD;
    *y = GAIT_THIGH_MM * sinf(r1) + GAIT_SHIN_MM * sinf(r2);
    *z = GAIT_THIGH_MM * cosf(r1) + GAIT_SHIN_MM * cosf(r2);
}

static void solve(float knee_deg, float *hip, float *knee)
{
    float h[LEG_COUNT], k[LEG_COUNT];
    gait_solve_static(knee_deg, NULL, NULL, h, k);
    *hip = h[0];
    *knee = k[0];
}

static float smoothstep(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

int main(void)
{
    float hip_s, knee_s, hip_l, knee_l;
    solve(GAIT_STAND_KNEE_DEG, &hip_s, &knee_s);
    solve(-95.0f, &hip_l, &knee_l);
    printf("站姿 hip=%.2f knee=%.2f  →  趴下 hip=%.2f knee=%.2f\n",
           hip_s, knee_s, hip_l, knee_l);
    printf("行程 hip %+.2f° / knee %+.2f°（比例 1:-2）\n\n",
           hip_l - hip_s, knee_l - knee_s);

    /* ---- 旧路径：逐关节等速限速 ---- */
    float h = hip_s, k = knee_s, drift_old = 0.0f;
    int hip_done = -1, knee_done = -1;
    for (int i = 0; i <= 10; i++) {
        float y, z;
        fk(h, k, &y, &z);
        if (fabsf(y) > drift_old) drift_old = fabsf(y);
        if (hip_done  < 0 && fabsf(h - hip_l)  < 0.01f) hip_done  = i;
        if (knee_done < 0 && fabsf(k - knee_l) < 0.01f) knee_done = i;
        float dh = hip_l - h, dk = knee_l - k;
        if (dh >  SLEW_DEG) dh =  SLEW_DEG;
        if (dh < -SLEW_DEG) dh = -SLEW_DEG;
        if (dk >  SLEW_DEG) dk =  SLEW_DEG;
        if (dk < -SLEW_DEG) dk = -SLEW_DEG;
        h += dh; k += dk;
    }
    printf("旧路径：脚前后最大漂移 %.2fmm；髋第 %d 拍到位、膝第 %d 拍到位（不同步）\n",
           drift_old, hip_done, knee_done);

    /* ---- 新路径：膝角 smoothstep + 每拍 IK 反解髋角 ---- */
    float drift_new = 0.0f, peak_rate = 0.0f, prev_k = knee_s;
    float last_hip = hip_s, last_knee = knee_s;
    for (int i = 0; i <= 50; i++) {
        float t = (float)(i * TICK_MS) / TRANS_MS;
        float kk = knee_s + (knee_l - knee_s) * smoothstep(t);
        float hh, kx;
        solve(kk, &hh, &kx);
        float y, z;
        fk(hh, kx, &y, &z);
        if (fabsf(y) > drift_new) drift_new = fabsf(y);
        float rate = fabsf(kk - prev_k) / (TICK_MS / 1000.0f);
        if (rate > peak_rate) peak_rate = rate;
        prev_k = kk;
        last_hip = hh; last_knee = kx;
    }
    printf("新路径：脚前后最大漂移 %.4fmm；峰值膝角速度 %.1f°/s；"
           "终点 hip=%.2f knee=%.2f\n\n", drift_new, peak_rate,
           last_hip, last_knee);

    int fail = 0;
#define CHECK(cond, msg) do { \
        printf("%s  %s\n", (cond) ? "PASS" : "FAIL", msg); \
        if (!(cond)) fail++; \
    } while (0)

    CHECK(drift_old > 15.0f, "旧路径确实漂了 >15mm（证明这个 bug 真实存在）");
    CHECK(hip_done < knee_done, "旧路径髋先到位、膝后到位（分段感来源）");
    CHECK(drift_new < 0.01f, "新路径脚前后位移 ≈ 0（机身纯垂直升降）");
    CHECK(peak_rate < 120.0f, "新路径峰值角速度 <120°/s（远离 600°/s 舵机极限）");
    CHECK(fabsf(last_hip - hip_l) < 0.01f && fabsf(last_knee - knee_l) < 0.01f,
          "新路径髋膝同一拍到位");

    printf("\n%s（%d 项失败）\n", fail ? "FAILED" : "ALL PASS", fail);
    return fail ? 1 : 0;
}
