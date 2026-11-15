/*
 * Spider8 蓝牙遥控入口（M3 落地）
 *
 * 板子作为 BLE 外设（GATT server），手机 app 连上后：
 *   - 在 CMD 特征值写 ASCII 命令（如 "walk 50\r\n" / "left"），板子执行；
 *   - 执行结果通过 STATUS 特征值（notify 推送）回给手机。
 *
 * 与有线串口、Wi-Fi 网页控制台完全共用同一套 cli_exec() 命令表，
 * 所以 app 能发的指令和电脑控制台一模一样。
 *
 * 安全说明（测试期）：当前为「开放模式」，连上即可发命令，不做配对鉴权。
 * 正式发布前建议加 BLE 配对（PIN / 数值比较）。见 docs/15-蓝牙遥控.md。
 */
#ifndef SPIDER8_BLE_CTL_H
#define SPIDER8_BLE_CTL_H

#include <stdbool.h>
#include "esp_err.h"

/* app_main 里在 wireless_init() 之后调用。失败不影响有线串口与 Wi-Fi 控制台。 */
esp_err_t ble_ctl_init(void);

/* 当前是否有手机 BLE 连接。wireless.c 的 httpd 看门狗必须查它：
   BLE 遥控期间若因网页自检失败就 esp_restart()，手机会立刻 status=8 断连，
   重启后重新广播又自动连上——表现为「一发指令过一会就断」。
   静态变量初值 false，wireless_init() 先于 ble_ctl_init() 调用也安全。 */
bool ble_ctl_is_connected(void);

#endif /* SPIDER8_BLE_CTL_H */
