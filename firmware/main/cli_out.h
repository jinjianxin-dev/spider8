/*
 * Spider8 命令输出 / 命令执行桥接层
 *
 * 解决的问题：原来所有命令用 printf 直接写 USB Serial/JTAG，
 * 无线通道收不到回显。这里把所有命令输出汇成三路：
 *   1. 有线 stdout（原行为，一字不改）
 *   2. 当前无线请求的捕获缓冲（请求-响应，点 status 立刻看到结果）
 *   3. 全局环形日志（无线网页轮询，也能看到 ESP_LOG 系统日志）
 *
 * REPL 与 HTTP 是两条独立入口，共用同一把执行锁；捕获用「任务句柄」
 * 归属判断，避免有线命令的输出串进无线请求的回显里。
 */
#ifndef SPIDER8_CLI_OUT_H
#define SPIDER8_CLI_OUT_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CLI_CMD_MAX      128    /* 与 REPL 的 max_cmdline_length 一致 */
#define CLI_OUT_MAX      4096   /* 单次命令返回给无线的最大文本长度 */

/* app_main 最早期调用：建执行锁 + 接管 ESP_LOG 输出到环形日志 */
esp_err_t cli_out_init(void);

/* 替代 printf：命令与所有需要回显的输出都走这里 */
void cli_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * 执行一行命令，并把这一行命令产生的输出写进 out。
 * 返回：0=命令执行成功；>0=命令自己返回的错误码；<0=桥接层错误。
 */
int cli_exec(const char *cmdline, char *out, size_t out_size);

/*
 * 读环形日志增量。since 是上次拿到的 now；返回本次的 now（累计字节数）。
 * 客户端落后超过缓冲区大小时自动只给最近的数据。
 */
uint32_t cli_log_read(uint32_t since, char *out, size_t out_size);

#endif /* SPIDER8_CLI_OUT_H */
