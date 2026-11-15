/*
 * Spider8 无线测试台（Wi-Fi AP + STA + HTTP）
 *
 * 定位：测试期的「无线串口线」，不是最终遥控形态。
 *   - AP 热点 spider8 常驻，手机 / 电脑连上即可发串口命令，也是配网入口
 *   - 网页里填一次家里 WiFi 后，固件同时以 STA 连路由器（APSTA 共存），
 *     电脑继续连家里网照常上网，用路由器分配的 IP 打开同一个控制台
 *   - 家里 WiFi 连不上不销毁任何 netif，热点照常可用；清除配网即回纯热点
 *   - 网页内嵌在固件里，完全离线可用
 *   - 有线 USB Serial/JTAG 完整保留，无线出问题时随时插线兜底
 *   - M3 的 BLE 小程序是另一条独立入口，与本模块不冲突
 *
 * 安全：网页每 500ms 发一次心跳；心跳断超过 2.5s 且当前在行走，
 *       自动回到站姿，避免关掉网页后机器狗一直往前跑。
 */
#ifndef SPIDER8_WIRELESS_H
#define SPIDER8_WIRELESS_H

#include "esp_err.h"

/* app_main 里在 console_init() 之后调用。失败不影响有线串口与运动功能。 */
esp_err_t wireless_init(void);

/* 有线诊断：插 USB 线敲 `wifidiag`，把热点状态与开机日志打到串口。
 * 手机/电脑搜不到热点时先用它判断是「固件没起 Wi-Fi」还是「射频/扫描问题」。 */
void wireless_diag(void);

/* 有线兜底：家里 WiFi 密码填错导致无线入口进不去时，敲 `wificlear`
 * 清掉 NVS 里的配网；断电重启后回到纯热点 spider8 / 192.168.4.1。
 * 只清配网命名空间，绝不碰舵机 trim。 */
esp_err_t wireless_clear_sta_config(void);

/* 有线写入：`wifiset <ssid> <pass>` 直接把家里 WiFi 配置写进 NVS
 * 并重启生效。网页配网入口连不上时（比如本机 VPN 抢路由）用它兜底。
 * 密码含空格时用引号包住；开放网络省略 pass 参数。 */
esp_err_t wireless_set_sta_config(const char *ssid, const char *pass);

#endif /* SPIDER8_WIRELESS_H */
