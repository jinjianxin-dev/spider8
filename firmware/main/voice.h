/*
 * Spider8 语音前端（I2：语音驱动动作）
 *
 * 链路：INMP441(I2S0) -> AFE -> WakeNet9「你好小智」-> MultiNet7 中文命令词
 *       -> 命令 id -> 队列 -> 白名单/安全门 -> cli_exec() -> 步态
 *
 * 【四道安全门】① 编译期常量白名单；② 运行时开关 exec，**上电默认 1（放行）**
 * （2026-11-16 用户拍板，喇叭与动作均已验收，要求重启免手动开闸），
 * `voice exec off` 可临时关闸、关闸瞬间回站姿；③ 四个「停」别名走
 * motion_stand() 急停直通，不等 2.5s 命令锁；④ 行走类命令 5s 自动站定。
 * 喇叭已修好，上电默认播报（mute=0）；⚠️ 上电即放行，把狗放地上前请先关闸。
 */
#ifndef SPIDER8_VOICE_H
#define SPIDER8_VOICE_H

#include <stdbool.h>

#include "esp_err.h"

/* 在 app_main 里、motion/console 都起来之后调用。
 * 失败只返回错误码，不 abort —— 语音是加分项，不能拖垮运动功能。 */
esp_err_t voice_init(void);

/* console.c 注册的 "voice" 命令入口：
 *   voice                    状态 + 内存 + 喇叭计数 + 栈水位 + 安全门状态
 *   voice list               打印命令词表（含 UNSUPPORTED 项）
 *   voice on / off           识别开关（音频流不停，只是不识别）
 *   voice exec on / off      **动作放行开关**，上电默认 on；off 时立即回站姿
 *   voice mute on / off      喇叭播报静音（上电默认 off=播报；on 时不闭麦）
 *   voice gain <12-20>       麦克风增益（右移量，越大音量越小；默认 16）
 *   voice sim <id>           不开口模拟一条命令，走完整安全门（exec=on 会动腿）
 *   voice say <clip>         喇叭自检（20=我在，19=好的，21=没听懂，18=已停止）
 */
int voice_cli(int argc, char **argv);

/* 语音前端是否已成功启动（供 status / 无线接口查询） */
bool voice_is_ready(void);

#endif /* SPIDER8_VOICE_H */
