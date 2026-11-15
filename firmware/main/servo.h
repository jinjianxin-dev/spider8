/*
 * Spider8 舵机驱动（输出后端可切换）
 *
 * 职责边界：
 *   - 本模块是【唯一】写 PWM 的地方，其他任务通过队列递交目标角
 *   - 输出后端编译期二选一（Kconfig.projbuild）：
 *       CONFIG_SERVO_BACKEND_LEDC     GPIO 直连（14bit）
 *       CONFIG_SERVO_BACKEND_PCA9685  I2C 扩展板（12bit，精度略粗）
 *     对上层完全同构：init / apply / 标定 / raw 电气验证行为一致
 *   - 输入是【关节角】（髋：0=竖直向下；膝：0=伸直），不是舵机角
 *   - 关节角 → 舵机角的映射（装配基准 90°、镜像 dir、trim）全部在这里做
 *     这样上层算法不用关心左右腿装反了没有
 */
#ifndef SPIDER8_SERVO_H
#define SPIDER8_SERVO_H

#include <stdbool.h>

#include "esp_err.h"

#include "config.h"

/* 初始化 8 路 LEDC，并把所有关节摆到给定的初始关节角（通常是站立角） */
esp_err_t servo_init(const float joint_deg[J_COUNT]);

/* 施加一组关节角。
 *   cal_mode = true 时跳过软限位（标定用），但仍受电子硬限位与 12°/拍 保护。 */
void servo_apply(const float joint_deg[J_COUNT], bool cal_mode);

/* 读回最近一次实际下发的值（调试/状态显示用，非精确同步） */
void servo_get_joint_deg(float out[J_COUNT]);   /* 关节角 */
void servo_get_servo_deg(float out[J_COUNT]);   /* 舵机角 0~180 */

/* 标定参数 */
void  servo_set_trim(int j, float deg);
float servo_get_trim(int j);
void  servo_set_dir(int j, int dir);
int   servo_get_dir(int j);
void  servo_get_limit(int j, float *lo, float *hi);

/* trim/dir 持久化到 NVS */
esp_err_t servo_save_cal(void);
esp_err_t servo_load_cal(void);

/* 关节角 → 舵机角（供状态显示、上位机核对） */
float servo_joint_to_servo(int j, float joint_deg);

/* 该关节对应的物理输出端（打印接线表用）：
 * LEDC 后端 = GPIO 号，PCA9685 后端 = 板上通道号 */
int servo_pin(int j);

/* 输出端文字标签，如 "GPIO4 " 或 "PCA0  "（printf %s 直接用） */
void servo_out_label(int j, char *buf, int len);

/* ================== 电气验证：原始脉宽输出 ==================
 * 用途：舵机还没到货时，用 LED + 电阻（或万用表 / 示波器）核对
 * 8 路 PWM 是否真的输出、脉宽是否随命令变化。
 *
 * raw 模式下 servo_apply() 被冻结 —— 步态和姿态都不再改写占空比，
 * 唯一的 PWM 来源变成 servo_raw_us()。这样呼吸灯才能稳定显示，
 * 而不是被 20ms 的舵机任务每拍覆盖掉。
 *
 * 退出（servo_raw_exit）后必须调用 motion_stand() 让姿态平滑回来：
 * (1) 单拍 12° 限速会从 raw 输出过的那个位置接续，不会突跳。
 * ============================================================ */
void     servo_raw_enter(void);
void     servo_raw_exit(void);
bool     servo_raw_active(void);

/* 直接输出指定脉宽（µs）。超出 300~2700µs 会被夹住。
 * 返回实际生效的 µs，便于串口打印核对。 */
uint32_t servo_raw_us(int j, int us);

/* 该关节当前不可达时的累计计数（clampN），用于判断"步幅超了腿长" */
uint32_t servo_clamp_count(void);

#endif /* SPIDER8_SERVO_H */
