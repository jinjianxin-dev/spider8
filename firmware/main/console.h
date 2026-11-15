/*
 * Spider8 串口命令行
 *
 * 没有 App、没有 BLE 的时候，这就是唯一的操作入口和验收手段。
 * 全部命令都只改"目标"，实际输出仍由 servo_out_task 统一写 LEDC。
 */
#ifndef SPIDER8_CONSOLE_H
#define SPIDER8_CONSOLE_H

#include "esp_err.h"

esp_err_t console_init(void);

#endif /* SPIDER8_CONSOLE_H */
