/*
 * Spider8 命令输出 / 命令执行桥接层实现
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_log.h"

#include "cli_out.h"

#define TAG             "cli_bridge"
#define CAP_SIZE        8192
#define LOG_SIZE        16384          /* 必须是 2 的幂 */
#define LINE_SIZE       512
#define EXEC_WAIT_MS    2500           /* 网页服务是单线程，排队等锁不能久占 */

static SemaphoreHandle_t s_exec_lock;
static TaskHandle_t      s_capture_task;
static char              s_cap[CAP_SIZE];
static size_t            s_cap_len;
static bool              s_capturing;

static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static char         s_log[LOG_SIZE];
static uint32_t     s_log_written;      /* 累计写入字节数，单调递增 */

/* ------------------------------------------------------------------ */
/* 环形日志                                                            */
/* ------------------------------------------------------------------ */

static void log_append(const char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    /* 临界区很短（最多 LINE_SIZE 次字节写），不在这里做任何阻塞操作 */
    portENTER_CRITICAL(&s_log_mux);
    for (size_t i = 0; i < len; i++) {
        s_log[s_log_written & (LOG_SIZE - 1)] = buf[i];
        s_log_written++;
    }
    portEXIT_CRITICAL(&s_log_mux);
}

/* ESP_LOG 也接进同一个环形日志，无线网页就能看到启动日志和报错 */
static int log_vprintf(const char *fmt, va_list args)
{
    char line[LINE_SIZE];

    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    /* 保持原有线输出，再入环形缓冲（vprintf 会消耗 args，所以必须先格式化） */
    int ret = vprintf(fmt, args);

    if (n > 0) {
        size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : (sizeof(line) - 1);
        log_append(line, len);
    }
    return ret;
}

esp_err_t cli_out_init(void)
{
    if (s_exec_lock == NULL) {
        s_exec_lock = xSemaphoreCreateMutex();
    }
    if (s_exec_lock == NULL) {
        ESP_LOGE(TAG, "执行锁创建失败");
        return ESP_ERR_NO_MEM;
    }
    esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 命令输出                                                            */
/* ------------------------------------------------------------------ */

void cli_out(const char *fmt, ...)
{
    char line[LINE_SIZE];

    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* 1) 有线 stdout：行为和改造前完全一致 */
    (void)vprintf(fmt, ap2);
    va_end(ap2);

    if (n <= 0) {
        return;
    }
    const size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : (sizeof(line) - 1);

    /* 2) 只捕获「发起这次无线执行」的那个任务的输出。
     *    REPL 任务即使并发执行，也不会把内容串进无线回显。 */
    if (s_capturing && s_capture_task == xTaskGetCurrentTaskHandle()) {
        const size_t room = (CAP_SIZE - 1) - s_cap_len;
        const size_t cp  = (len < room) ? len : room;
        if (cp > 0) {
            memcpy(s_cap + s_cap_len, line, cp);
            s_cap_len += cp;
            s_cap[s_cap_len] = '\0';
        }
    }

    /* 3) 全局日志（网页 /api/log 轮询） */
    log_append(line, len);
}

/* ------------------------------------------------------------------ */
/* 命令执行                                                            */
/* ------------------------------------------------------------------ */

static void append_msg(char *out, size_t size, const char *msg)
{
    const size_t cur = strlen(out);
    if (cur + 1 >= size) {
        return;
    }
    snprintf(out + cur, size - cur, "%s", msg);
}

int cli_exec(const char *cmdline, char *out, size_t out_size)
{
    if (s_exec_lock == NULL || cmdline == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    if (xSemaphoreTake(s_exec_lock, pdMS_TO_TICKS(EXEC_WAIT_MS)) != pdTRUE) {
        append_msg(out, out_size, "上一条命令还没执行完，请稍后重试（网页服务同一时间只跑一条命令）\n");
        return -2;
    }

    s_capture_task = xTaskGetCurrentTaskHandle();
    s_cap_len = 0;
    s_cap[0] = '\0';
    s_capturing = true;

    int cmd_ret = 0;
    esp_err_t err = esp_console_run(cmdline, &cmd_ret);

    s_capturing = false;
    s_capture_task = NULL;

    const size_t n = (s_cap_len < out_size - 1) ? s_cap_len : (out_size - 1);
    const bool had_output = (s_cap_len > 0);
    memcpy(out, s_cap, n);
    out[n] = '\0';

    xSemaphoreGive(s_exec_lock);

    /* esp_console 自己打印的提示（未知命令、help 等）不走 cli_out，
     * 这里补上，否则无线侧会看到一片空白却不知道发生了什么。 */
    if (err == ESP_OK && !had_output) {
        append_msg(out, out_size,
            "（这条命令的输出由 esp_console 内部直接打印，无线侧抓不到，\n"
            "  常见的是 help。完整命令表见网页「命令速查」，或看有线串口输出）\n");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        append_msg(out, out_size, "未知命令（输入 help 查看命令表；网页左侧有命令速查）\n");
    } else if (err == ESP_ERR_INVALID_ARG) {
        append_msg(out, out_size, "命令为空或只有空白字符\n");
    } else if (err == ESP_ERR_INVALID_STATE) {
        append_msg(out, out_size, "命令表未初始化（串口 REPL 未启动），请检查 USB Serial/JTAG\n");
    } else if (err != ESP_OK) {
        append_msg(out, out_size, "命令执行异常\n");
    }

    if (err != ESP_OK) {
        return -3;
    }
    return cmd_ret;
}

uint32_t cli_log_read(uint32_t since, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    uint32_t now;
    portENTER_CRITICAL(&s_log_mux);
    now = s_log_written;
    portEXIT_CRITICAL(&s_log_mux);

    const uint32_t oldest = (now > LOG_SIZE) ? (now - LOG_SIZE) : 0;
    uint32_t start = (since < oldest) ? oldest : since;
    if (start > now) {
        start = now;
    }

    size_t want = (size_t)(now - start);
    if (want > out_size - 1) {
        want = out_size - 1;
        start = now - (uint32_t)want;
    }

    portENTER_CRITICAL(&s_log_mux);
    /* 若在读的过程中又有新日志写入，按开始时的 now 截断，保证 since 语义稳定 */
    for (size_t i = 0; i < want; i++) {
        out[i] = s_log[(start + (uint32_t)i) & (LOG_SIZE - 1)];
    }
    portEXIT_CRITICAL(&s_log_mux);

    out[want] = '\0';
    return now;
}
