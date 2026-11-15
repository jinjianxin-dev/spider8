/*
 * Spider8 运动管理层
 *
 * 三个来源，一份输出：
 *   MODE_WALK   步态引擎（走路/小跑）
 *   MODE_POSE   静态位姿（站立/蹲下/趴下/握手/挥手）
 *   MODE_MANUAL 串口手动标定（单关节点动）
 *
 * 统一出口是「8 个关节角」，经斜率限制后投递给 servo_out_task。
 * 斜率限制是这套东西能平稳运行的关键：任何模式切换、参数突变，
 * 最终都是 20ms 最多走 11°，舵机永远不会收到跳变指令。
 *
 * POSE 模式另有专用缓动：机身高度只由「膝角」一个标量决定，髋角每拍
 * 由 IK 反解（髋 = −膝/2），保证脚不前后滑、髋膝同时到位。详见 config.h。
 */
#ifndef SPIDER8_MOTION_H
#define SPIDER8_MOTION_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "config.h"
#include "gait.h"

typedef enum {
    POSE_STAND = 0,   /* 站立（基准姿态） */
    POSE_CROUCH,      /* 蹲下 */
    POSE_LIE,         /* 趴下 */
    POSE_SHAKE,       /* 握手（右前腿抬起前伸） */
    POSE_WAVE,        /* 挥手（右前腿抬起上下摆） */
    POSE_COUNT
} pose_id_t;

typedef enum {
    MODE_POSE = 0,
    MODE_WALK,
    MODE_MANUAL
} motion_mode_t;

/* 纯函数：算站立姿态的 8 个关节角，不需要初始化。供 servo_init 用。 */
void motion_stand_angles(float out[J_COUNT]);

esp_err_t motion_init(void);
esp_err_t motion_start(void);

/* ---- 模式切换（全部是「请求」：先经中间态站姿，再执行目标） ---- */
void motion_stand(void);
void motion_pose(pose_id_t id);
void motion_walk(uint8_t pattern, int speed_pct);
void motion_spin(int dir);      /* 原地左/右转：dir<0 左转、>0 右转，靠左右步幅差动 */
void motion_straight(void);     /* 转向回正 + 恢复直行 */

/* ---- 参数在线调整（立即生效，下一拍开始） ---- */
void motion_set_speed(int pct);      /* 0~100 */
void motion_set_turn(int pct);       /* -100~+100 */
void motion_set_period(int ms);      /* 800~3000 */
void motion_set_stride(float mm);    /* 8~30 */
void motion_set_lift(float mm);      /* 6~26 */
void motion_set_duty(int pct);       /* 10~45，波步 ≤25 */
void motion_set_pose_trans(int ms);  /* 姿态过渡时长 200~3000ms，0/无参=查询 */
void motion_set_fade_trans(int ms);  /* 收脚/起步淡入淡出时长 200~2000ms */
void motion_set_settle_trans(int ms); /* 站姿停留时长 100~2000ms，等舵机真正站稳 */
void motion_set_stand_knee(float deg); /* 站立屈膝角 -70..-30，越大越弯；不动 trim */

/* ---- 手动标定 ---- */
void motion_manual(int joint, float joint_deg);

/* ---- 状态查询 ---- */
const gait_params_t *motion_params(void);
motion_mode_t        motion_mode(void);
pose_id_t            motion_current_pose(void);
const char          *motion_mode_name(void);
const char          *motion_pose_name(pose_id_t id);
const char          *motion_trans_name(void);      /* 就绪/回站姿中/站姿停留中/起步淡入中 */
const char          *motion_pending_name(void);    /* 过渡期间挂起的目标动作 */
void                 motion_get_out(float out[J_COUNT]);
uint32_t             motion_tick_count(void);
uint32_t             motion_pose_trans_ms(void);
uint32_t             motion_fade_trans_ms(void);
uint32_t             motion_settle_trans_ms(void);

/* 任务栈剩余量（字），用于验证栈是"算出来的"而不是"猜的" */
void motion_diag(uint32_t *motion_hwm_words, uint32_t *servo_hwm_words);

#endif /* SPIDER8_MOTION_H */
