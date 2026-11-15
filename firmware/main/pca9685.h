/*
 * PCA9685 16 路 PWM 扩展板驱动（舵机后端之一）
 *
 * 芯片要点：
 *   - I2C 从机地址 0x40（板上 6 个地址跳线焊点全断开时）
 *   - 12bit 计数器，50Hz 下一周期 4096 计数 → 1 计数 ≈ 4.88µs ≈ 0.44°
 *     （比 LEDC 的 14bit 粗，trim 标定时每步约 0.44°）
 *   - 每通道 4 个寄存器：LEDn_ON_L/H（上升沿计数）、LEDn_OFF_L/H（下降沿计数）
 *     舵机用法：ON 固定为 0，OFF = 脉宽折算的计数 → 正脉冲宽度 = 脉宽
 *   - 改 PRESCALE 必须先 MODE1.SLEEP=1（bit4），否则写入被忽略，
 *     芯片会一直跑上电默认的 PRESCALE=30（200Hz），舵机完全不认
 *   - MODE1.AI(bit5) = 自动地址递增（4 字节连写/连读必需）；
 *     MODE2.OUTDRV(bit2) = 推挽；MODE2.INVRT(bit4) 必须保持 0，置 1 极性反转
 *   - AI 在 MODE1，不在 MODE2；MODE2 的 0x10 是 INVRT 不是 AI（曾踩坑）
 *
 * 写入频率：servo_out_task 每 20ms 调一次，单通道 5 字节 ≈ 0.1ms@400kHz，
 * 8 通道全刷约 0.9ms，占 20ms 预算的 4.5%，余量充足。
 *
 * 线程安全：servo_out_task 和串口 raw 命令都可能写 → 内部互斥锁保护。
 */
#ifndef SPIDER8_PCA9685_H
#define SPIDER8_PCA9685_H

#include <stdint.h>

#include "esp_err.h"

#include "config.h"

/* 初始化 I2C 总线 + PCA9685（50Hz、推挽、自动递增）。
 * 会探测器件应答：总线上找不到 0x40 直接报错返回，避免静默失败。 */
esp_err_t pca9685_init(void);

/* 单通道写脉宽。us 先夹到 0~4095 计数（≈0~4096µs 内不会溢出 12bit）。
 * 返回 ESP_OK 表示 I2C 已确认写入。 */
esp_err_t pca9685_write_us(int ch, uint32_t us);

/* 运行时诊断：读回 MODE1 / MODE2 / PRESCALE / LED0 四组寄存器并打印。
 * 用于定位「I2C 写得进去、status 也正常，但输出脚恒 3.3V / 全 0」这类故障：
 *   - MODE1.SLEEP=1  → 计数器没跑，输出锁死
 *   - MODE1.AI=0     → 4 字节连写塌到同一寄存器，ON=OFF，输出恒定电平
 *   - MODE2.INVRT=1  → PWM 极性反转
 *   - PRESCALE≠121   → 频率不是 50Hz（30 = 出厂默认 200Hz）
 *   - LED0.OFF 没变  → 脉宽根本没写进通道寄存器
 * OE 是硬件引脚、寄存器读不到，需另外用万用表量（对 GND 应 ≈0V）。 */
esp_err_t pca9685_diag(void);

#endif /* SPIDER8_PCA9685_H */
