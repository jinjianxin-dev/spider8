/*
 * Spider8 步态引擎 —— 两连杆逆运动学 + 相位步态
 *
 * 与 tools/gait_sim/gait_core.js 一一对应，改动请两边同步。
 * 纯计算模块：不碰外设、不分配内存、无全局状态，可以在 PC 上单测。
 */
#ifndef SPIDER8_GAIT_H
#define SPIDER8_GAIT_H

#include <stdint.h>
#include <stdbool.h>

#include "config.h"

/* 步态相位表：256 = 一个完整周期，四条腿各偏移多少 */
typedef enum {
    GAIT_PATTERN_WALK = 0,  /* 波步：四拍，任意时刻 3 条腿着地，最稳 */
    GAIT_PATTERN_TROT,      /* 小跑：对角同步，2 条支撑，快但需要惯性 */
    GAIT_PATTERN_PACE,      /* 溜蹄：同侧同步，会左右摇，只做演示 */
    GAIT_PATTERN_BOUND,     /* 跳跃：前后成对，最激进 */
    GAIT_PATTERN_COUNT
} gait_pattern_t;

typedef struct {
    uint16_t period_ms;     /* 步态周期，800~3000ms（低于 800 舵机跟不上） */
    uint8_t  duty_pct;      /* 摆动相占比，10~45；波步必须 ≤25 */
    float    stride_mm;     /* 步幅半幅：脚在髋点前后 ±stride */
    float    lift_mm;       /* 摆动相抬脚高度 */
    float    stand_knee_deg;/* 站立屈膝角，决定机身高度 */
    int8_t   speed_pct;     /* 0~100，线性缩放步幅；0 = 原地踏步 */
    int8_t   turn_pct;      /* -100~+100，左右步幅差动，>0 右转 */
    uint8_t  pattern;       /* gait_pattern_t */
} gait_params_t;

typedef struct {
    float   hip_deg[LEG_COUNT];   /* 关节角：0 = 竖直向下，+ = 向前 */
    float   knee_deg[LEG_COUNT];  /* 关节角：0 = 伸直，负 = 向后折 */
    uint8_t phase;                /* 全局相位 0~255 */
    bool    swinging[LEG_COUNT];
} gait_out_t;

/* ---- 运动学 ---------------------------------------------------------- */

/* 屈膝角 → 髋角：保证脚落在髋点正下方（否则四脚整体前后偏移，重心出支撑面） */
float gait_hip_for_knee(float knee_deg);

/* 屈膝角 → 髋轴离地高度 mm（脚刚好落地） */
float gait_height_for_knee(float knee_deg);

/* 两连杆逆运动学
 *   y_mm     脚相对髋点的前后位置（+ 前）
 *   zdown_mm 脚相对髋点的竖直向下深度
 * 输出关节角；clamped: 0 可达 / +1 太远被拉直 / -1 太近被顶住 */
void gait_ik2(float y_mm, float zdown_mm,
              float *hip_deg, float *knee_deg, int8_t *clamped);

/* ---- 步态 ------------------------------------------------------------ */

/* 解算某一时刻的四条腿。
 *   tick            本周期内第几个控制拍（0..ticks_per_cycle-1）
 *   ticks_per_cycle period_ms / GAIT_TICK_MS
 * 相位用整数推进，天然避免浮点边界抖动导致"两条腿同时抬起"。 */
void gait_step(const gait_params_t *p, uint32_t tick,
               uint32_t ticks_per_cycle, gait_out_t *out);

/* 动作切换淡入/淡出：fade=0 → 四脚中立（纯站姿几何），fade=1 → 与 src 完全一致。
 * 只缩放 speed_pct / turn_pct；stride_mm 保持不变，因为它同时是 gait_step
 * 里抬脚幅值的归一化分母，缩到 0 会除零。纯函数，可在 PC 上单测。 */
void gait_apply_fade(const gait_params_t *src, float fade, gait_params_t *dst);

/* 参数可行性估算：跑一个完整周期，给出峰值关节角速度与标称前进量。
 * MG90S 上限 600°/s，超了就是"指令角跑到机械角前面"——脚会绊。
 * 串口 status 用它做红黄绿灯，避免参数调到舵机跟不上还以为是机械问题。 */
void gait_estimate(const gait_params_t *p,
                   float *max_rate_dps, float *mm_per_cycle);

/* 静态位姿：给定屈膝角 + 每腿的额外前后偏移/抬脚高度，解出 8 个关节角。
 * 蹲下、趴下、握手都走这条路径 —— 机身高度由屈膝角决定，脚永远踩地。 */
void gait_solve_static(float knee_deg,
                       const float foot_y[LEG_COUNT],
                       const float foot_lift[LEG_COUNT],
                       float hip_out[LEG_COUNT],
                       float knee_out[LEG_COUNT]);

const char *gait_pattern_name(uint8_t pattern);

#endif /* SPIDER8_GAIT_H */
