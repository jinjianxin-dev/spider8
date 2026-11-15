/*
 * Spider8 无线测试台：Wi-Fi AP + STA + HTTP + 简易 DNS（强制门户）+ 断连急停
 *
 * 命令一条都不重写：HTTP 收到文字后交给 cli_exec() → esp_console_run()，
 * 与有线 REPL 走的是同一张命令表、同一套 motion/servo 代码。
 *
 * 两种网络角色同时在线（WIFI_MODE_APSTA）：
 *   - AP 热点 spider8 / 192.168.4.1：永远在，是配网入口与断网兜底
 *   - STA 站：NVS 里配了家里 WiFi 就连路由器，电脑/手机照常上网，
 *             再用路由器分配的 IP 访问同一个网页控制台
 *   STA 连不上不销毁/重建任何 netif，热点照常可用，因此没有回退风险。
 *
 * 参考 ESP-IDF 官方 captive_portal 示例（CC0/Unlicense）的 404→303 思路，
 * DNS 应答器按本项目需要重写为静态分配 + 带超时的精简版，不依赖托管组件。
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "wireless.h"

#define TAG              "wireless"

#ifdef CONFIG_SPIDER8_WIFI_ENABLED

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "esp_http_server.h"
#include "lwip/sockets.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "ble_ctl.h"
#include "cli_out.h"
#include "gait.h"
#include "motion.h"
#include "servo.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#define WD_TIMEOUT_MS    2500
#define WD_POLL_MS       250
#define DNS_PORT         53
#define DNS_BUF          256
#define HTTP_QUERY_MAX   1024   /* 配网密码 URL 编码后可能较长 */
#define AP_NETIF_KEY     "WIFI_AP_DEF"
#define FALLBACK_AP_IP   0x0104A8C0u   /* 192.168.4.1，按网络字节序读出 */
#define NVS_NS           "spider8_wifi"
#define WIFI_SSID_MAX    33            /* 32 字节 + '\0' */
#define WIFI_PASS_MAX    65            /* WPA2 最长 63 字节 + '\0' */
#define STA_MAX_RETRY    3             /* 断连后有限重试，不无限打日志 */
#define STA_HOSTNAME     "spider8"     /* 交给路由器 DHCP，便于按名字访问 */
#define NET_JSON_MAX     768

/* 网页：构建时由 EMBED_FILES 嵌进固件，符号名里的 '.' 会变成 '_' */
extern const char console_html_start[] asm("_binary_console_html_start");
extern const char console_html_end[]   asm("_binary_console_html_end");

static httpd_handle_t s_server;
static bool s_ota_in_progress = false;   /* OTA 上传进行中：看门狗自检需跳过的标志 */
static esp_netif_t   *s_ap_netif;
static esp_netif_t   *s_sta_netif;
static volatile int   s_sta_count;
static volatile uint32_t s_last_ping_tick;
static volatile bool  s_wd_armed;
static uint32_t       s_ap_ip_net;
static uint32_t       s_sta_ip_net;
static uint32_t       s_gw_ip_net;
static volatile bool  s_sta_up;
static bool           s_sta_configured;
static int            s_sta_retry;
static int8_t         s_sta_rssi;
static char           s_sta_ssid[WIFI_SSID_MAX];
static volatile bool  s_ap_muted;   /* STA 已连家里路由，热点被自动关闭 */

/* ------------------------------------------------------------------ */
/* 响应头小工具（每个返回值都检查，httpd 出错时不静默）                  */
/* ------------------------------------------------------------------ */

static void resp_hdr(httpd_req_t *req, const char *key, const char *val)
{
    esp_err_t e = httpd_resp_set_hdr(req, key, val);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "响应头 %s 设置失败: %s", key, esp_err_to_name(e));
    }
}

static void resp_type(httpd_req_t *req, const char *type)
{
    esp_err_t e = httpd_resp_set_type(req, type);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Content-Type 设置失败: %s", esp_err_to_name(e));
    }
}

static void resp_status(httpd_req_t *req, const char *status)
{
    esp_err_t e = httpd_resp_set_status(req, status);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "状态码设置失败: %s", esp_err_to_name(e));
    }
}

/* 网页既能从 192.168.4.1 内嵌打开，也能双击本地 html 文件打开（file://），
 * 后者的 fetch 属于跨源请求，必须放通。自定义头 X-Log-Now 也要显式暴露。 */
static void resp_cors(httpd_req_t *req)
{
    resp_hdr(req, "Access-Control-Allow-Origin", "*");
    resp_hdr(req, "Access-Control-Expose-Headers", "X-Log-Now");
    resp_hdr(req, "Cache-Control", "no-store");
    /* 关键：每次响应后立刻断开。esp_http_server 的会话槽只有个位数，
       浏览器若复用 keep-alive 连接，槽位会被占满；此时新连接进来只能被
       直接 RST，表现就是「电脑和手机都打不开板子网页」。 */
    resp_hdr(req, "Connection", "close");
}

/* ------------------------------------------------------------------ */
/* URL 参数                                                            */
/* ------------------------------------------------------------------ */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t size)
{
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 1 < size; p++) {
        if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
            int hi = hex_val(p[1]);
            int lo = hex_val(p[2]);
            if (hi >= 0 && lo >= 0) {
                dst[o++] = (char)((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        dst[o++] = (*p == '+') ? ' ' : *p;
    }
    dst[o] = '\0';
}

/* httpd_query_key_value() 不做百分号解码，命令里的空格得自己还原 */
static esp_err_t query_get(httpd_req_t *req, const char *key, char *val, size_t size)
{
    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (qlen >= HTTP_QUERY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char q[HTTP_QUERY_MAX];
    esp_err_t e = httpd_req_get_url_query_str(req, q, sizeof(q));
    if (e != ESP_OK) {
        return e;
    }

    char raw[HTTP_QUERY_MAX];
    e = httpd_query_key_value(q, key, raw, sizeof(raw));
    if (e != ESP_OK) {
        return e;
    }
    url_decode(raw, val, size);
    return ESP_OK;
}

static void beat(void)
{
    s_last_ping_tick = xTaskGetTickCount();
    s_wd_armed = true;
}

/* ------------------------------------------------------------------ */
/* 配网：家里 WiFi 的 SSID / 密码存 NVS                                 */
/* 命名空间独立于舵机标定值，清除配网不会碰 trim。                      */
/* ------------------------------------------------------------------ */

static void ip_str(uint32_t addr_net, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u",
             (unsigned)(addr_net & 0xFFu),
             (unsigned)((addr_net >> 8) & 0xFFu),
             (unsigned)((addr_net >> 16) & 0xFFu),
             (unsigned)((addr_net >> 24) & 0xFFu));
}

/* SSID 里可能有引号、反斜杠，直接拼进 JSON 会破坏结构 */
static void json_str(const char *src, char *dst, size_t size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src;
         *p != '\0' && o + 7 < size; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)*p;
        } else if (*p < 0x20) {
            snprintf(dst + o, size - o, "\\u%04x", (unsigned)*p);
            o += 6;
        } else {
            dst[o++] = (char)*p;
        }
    }
    dst[o] = '\0';
}

static esp_err_t net_cfg_load(char *ssid, size_t ssid_size,
                              char *pass, size_t pass_size)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        return e;                       /* 命名空间不存在 = 还没配过网 */
    }
    size_t ls = ssid_size, lp = pass_size;
    const esp_err_t es = nvs_get_str(h, "ssid", ssid, &ls);
    const esp_err_t ep = nvs_get_str(h, "pass", pass, &lp);
    nvs_close(h);
    if (es != ESP_OK || ep != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t net_cfg_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t net_cfg_clear(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static bool net_cred_valid(const char *ssid, const char *pass)
{
    const size_t ls = strlen(ssid), lp = strlen(pass);
    if (ls == 0 || ls > 32) return false;
    if (lp != 0 && (lp < 8 || lp > 63)) return false;
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++) {
        if (iscntrl(*p)) return false;
    }
    for (const unsigned char *p = (const unsigned char *)pass; *p; p++) {
        if (iscntrl(*p)) return false;
    }
    return true;
}

/* 让 HTTP 响应先发完再重启，否则网页只看到连接被重置 */
static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "配网已更新，重启以应用新的 Wi-Fi 配置");
    esp_restart();
    vTaskDelete(NULL);                 /* esp_restart 不返回，仅为静态分析补齐 */
}

static esp_err_t schedule_restart(void)
{
    return (xTaskCreate(delayed_restart_task, "net_reboot", 2048, NULL, 3, NULL) == pdPASS)
        ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------------------ */
/* HTTP 接口                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t h_root(httpd_req_t *req)
{
    resp_type(req, "text/html; charset=utf-8");
    resp_cors(req);
    const size_t len = (size_t)(console_html_end - console_html_start);
    esp_err_t e = httpd_resp_send(req, console_html_start, len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "网页发送失败: %s", esp_err_to_name(e));
    }
    return e;
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    char cmd[CLI_CMD_MAX + 1] = { 0 };
    if (query_get(req, "c", cmd, sizeof(cmd)) != ESP_OK || cmd[0] == '\0') {
        resp_status(req, "400 Bad Request");
        resp_type(req, "text/plain; charset=utf-8");
        resp_cors(req);
        return httpd_resp_send(req, "缺少命令参数 c，或命令为空", HTTPD_RESP_USE_STRLEN);
    }

    beat();
    ESP_LOGI(TAG, "无线命令: %s", cmd);

    char out[CLI_OUT_MAX];
    const int ret = cli_exec(cmd, out, sizeof(out));

    resp_status(req, (ret == 0) ? "200 OK" : "400 Bad Request");
    resp_type(req, "text/plain; charset=utf-8");
    resp_cors(req);
    esp_err_t e = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "命令回显发送失败: %s", esp_err_to_name(e));
    }
    return e;
}

/* 急停不走命令表、不等执行锁：哪怕 pwmscan 正跑 19 秒也能立刻停 */
static esp_err_t h_stop(httpd_req_t *req)
{
    beat();
    motion_stand();
    ESP_LOGW(TAG, "无线急停：回到站立姿态");
    resp_type(req, "text/plain; charset=utf-8");
    resp_cors(req);
    return httpd_resp_send(req, "已急停，回到站立姿态", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_ping(httpd_req_t *req)
{
    beat();
    resp_type(req, "text/plain; charset=utf-8");
    resp_cors(req);
    return httpd_resp_send(req, "pong", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    const gait_params_t *p = motion_params();
    float rate = 0.0f, per_cycle = 0.0f;
    gait_estimate(p, &rate, &per_cycle);

    const uint32_t dt_ms = (xTaskGetTickCount() - s_last_ping_tick) * portTICK_PERIOD_MS;

    char json[1024];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"pose\":\"%s\",\"gait\":\"%s\",\"trans\":\"%s\","
        "\"period\":%u,\"duty\":%d,\"stride\":%.1f,\"lift\":%.1f,"
        "\"speed\":%d,\"turn\":%d,\"hold\":%u,\"rate\":%.0f,\"clamp\":%lu,"
        "\"heap\":%lu,\"heap_min\":%lu,\"uptime\":%lu,"
        "\"clients\":%d,\"hb\":%lu,\"wd\":%d,\"ota\":%d,"
        "\"rst\":%d,\"brownout\":%d}",
        motion_mode_name(), motion_pose_name(motion_current_pose()),
        gait_pattern_name(p->pattern), motion_trans_name(),
        (unsigned)p->period_ms, (int)p->duty_pct,
        (double)p->stride_mm, (double)p->lift_mm,
        (int)p->speed_pct, (int)p->turn_pct,
        (unsigned)motion_settle_trans_ms(), (double)rate,
        (unsigned long)servo_clamp_count(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)(esp_log_timestamp() / 1000),
        (int)s_sta_count, (unsigned long)dt_ms, s_wd_armed ? 1 : 0, (int)s_ota_in_progress,
        /* 上次复位原因：诊断「手机一发指令就断、随后自动连上」是否是板子
           掉压/崩溃重启。brownout=1 即舵机启动把 5V 拉塌，属电源问题。 */
        (int)esp_reset_reason(), (esp_reset_reason() == ESP_RST_BROWNOUT) ? 1 : 0);

    if (n < 0 || n >= (int)sizeof(json)) {
        resp_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "status buffer overflow", HTTPD_RESP_USE_STRLEN);
    }
    resp_type(req, "application/json; charset=utf-8");
    resp_cors(req);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_log(httpd_req_t *req)
{
    char since_str[24] = { 0 };
    (void)query_get(req, "since", since_str, sizeof(since_str));
    const uint32_t since = (uint32_t)strtoul(since_str, NULL, 10);

    char out[CLI_OUT_MAX];
    const uint32_t now = cli_log_read(since, out, sizeof(out));

    char now_str[24];
    snprintf(now_str, sizeof(now_str), "%lu", (unsigned long)now);
    resp_hdr(req, "X-Log-Now", now_str);
    resp_type(req, "text/plain; charset=utf-8");
    resp_cors(req);
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* 网络状态：网页靠它显示「连家里路由时该打开哪个地址」 */
static esp_err_t h_net(httpd_req_t *req)
{
    char ap_ip[16] = "0.0.0.0";
    char sta_ip[16] = "0.0.0.0";
    char gw_ip[16]  = "0.0.0.0";
    char console_url[40];
    char sta_ssid_esc[WIFI_SSID_MAX * 2];
    char ap_ssid_esc[64];

    if (s_ap_ip_net != 0) ip_str(s_ap_ip_net, ap_ip, sizeof(ap_ip));
    if (s_sta_up && s_sta_ip_net != 0) {
        ip_str(s_sta_ip_net, sta_ip, sizeof(sta_ip));
        ip_str(s_gw_ip_net, gw_ip, sizeof(gw_ip));
        snprintf(console_url, sizeof(console_url), "http://%s", sta_ip);
    } else {
        snprintf(console_url, sizeof(console_url), "http://%s", ap_ip);
    }
    json_str(s_sta_ssid, sta_ssid_esc, sizeof(sta_ssid_esc));
    json_str(CONFIG_SPIDER8_WIFI_SSID, ap_ssid_esc, sizeof(ap_ssid_esc));

    if (s_sta_up) {
        wifi_ap_record_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_sta_rssi = ap_info.rssi;
        }
    }

    char json[NET_JSON_MAX];
    const int n = snprintf(json, sizeof(json),
        "{\"ap\":%d,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
        "\"sta_configured\":%d,\"sta_ssid\":\"%s\",\"sta_up\":%d,"
        "\"sta_ip\":\"%s\",\"gw\":\"%s\",\"rssi\":%d,"
        "\"hostname\":\"%s\",\"console\":\"%s\",\"retry\":%d}",
        s_ap_muted ? 0 : 1, ap_ssid_esc, ap_ip,
        s_sta_configured ? 1 : 0, sta_ssid_esc, s_sta_up ? 1 : 0,
        sta_ip, gw_ip, (int)s_sta_rssi,
        STA_HOSTNAME, console_url, s_sta_retry);

    if (n < 0 || n >= (int)sizeof(json)) {
        resp_status(req, "500 Internal Server Error");
        resp_cors(req);
        return httpd_resp_send(req, "net json overflow", HTTPD_RESP_USE_STRLEN);
    }
    resp_type(req, "application/json; charset=utf-8");
    resp_cors(req);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* 配网：保存 / 清除家里 WiFi，随后统一重启生效（重启不擦舵机 trim） */
static esp_err_t h_wifi(httpd_req_t *req)
{
    char action[16] = { 0 };
    if (query_get(req, "action", action, sizeof(action)) != ESP_OK) {
        resp_status(req, "400 Bad Request");
        resp_type(req, "text/plain; charset=utf-8");
        resp_cors(req);
        return httpd_resp_send(req, "缺少 action 参数", HTTPD_RESP_USE_STRLEN);
    }

    if (strcmp(action, "save") == 0) {
        char ssid[WIFI_SSID_MAX] = { 0 };
        char pass[WIFI_PASS_MAX] = { 0 };
        if (query_get(req, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
            query_get(req, "pass", pass, sizeof(pass)) != ESP_OK ||
            !net_cred_valid(ssid, pass)) {
            resp_status(req, "400 Bad Request");
            resp_type(req, "text/plain; charset=utf-8");
            resp_cors(req);
            return httpd_resp_send(req,
                "SSID 需 1~32 字符；密码留空（开放网络）或 8~63 字符",
                HTTPD_RESP_USE_STRLEN);
        }

        esp_err_t e = net_cfg_save(ssid, pass);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "配网写入 NVS 失败: %s", esp_err_to_name(e));
            resp_status(req, "500 Internal Server Error");
            resp_type(req, "text/plain; charset=utf-8");
            resp_cors(req);
            return httpd_resp_send(req, "NVS 写入失败，配置未保存", HTTPD_RESP_USE_STRLEN);
        }
        e = schedule_restart();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "重启任务创建失败: %s", esp_err_to_name(e));
            resp_status(req, "500 Internal Server Error");
            resp_type(req, "text/plain; charset=utf-8");
            resp_cors(req);
            return httpd_resp_send(req, "已保存但无法自动重启，请手动断电重启",
                                   HTTPD_RESP_USE_STRLEN);
        }
        resp_type(req, "text/plain; charset=utf-8");
        resp_cors(req);
        return httpd_resp_send(req,
            "已保存，板子 1.5 秒后重启。热点会一直保留；若连不上家里路由，仍可从 http://192.168.4.1 打开本页修改。",
            HTTPD_RESP_USE_STRLEN);
    }

    if (strcmp(action, "clear") == 0) {
        esp_err_t e = net_cfg_clear();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "清除配网失败: %s", esp_err_to_name(e));
            resp_status(req, "500 Internal Server Error");
            resp_type(req, "text/plain; charset=utf-8");
            resp_cors(req);
            return httpd_resp_send(req, "NVS 清除失败", HTTPD_RESP_USE_STRLEN);
        }
        e = schedule_restart();
        if (e != ESP_OK) {
            resp_status(req, "500 Internal Server Error");
            resp_type(req, "text/plain; charset=utf-8");
            resp_cors(req);
            return httpd_resp_send(req, "已清除但无法自动重启，请手动断电重启",
                                   HTTPD_RESP_USE_STRLEN);
        }
        resp_type(req, "text/plain; charset=utf-8");
        resp_cors(req);
        return httpd_resp_send(req, "已清除家里 WiFi 配置，重启后只保留机器狗热点",
                               HTTPD_RESP_USE_STRLEN);
    }

    resp_status(req, "400 Bad Request");
    resp_type(req, "text/plain; charset=utf-8");
    resp_cors(req);
    return httpd_resp_send(req, "action 只能是 save 或 clear", HTTPD_RESP_USE_STRLEN);
}

/* 手机连上「无互联网」热点后会访问 /generate_204 之类探测地址，
 * 全部 303 到控制台，iOS/Android 就会弹出登录页 = 我们的控制台。 */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    resp_status(req, "303 See Other");
    resp_hdr(req, "Location", "/");
    resp_hdr(req, "Connection", "close");
    return httpd_resp_send(req, "Redirect to spider8 console", HTTPD_RESP_USE_STRLEN);
}

/* 网页服务自检专用：只回一个词，且不喂心跳（否则断连急停会被误判为一直在线） */
static esp_err_t h_guard(httpd_req_t *req)
{
    resp_type(req, "text/plain; charset=utf-8");
    resp_hdr(req, "Connection", "close");
    return httpd_resp_send(req, "alive", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* OTA 固件无线升级：POST 原始 .bin 到 /api/update，写进空闲 OTA 槽，
   校验通过后设置下次启动分区并重启。之后改固件不用再插 USB 线。
   上传期间 s_ota_in_progress=true，httpd 看门狗自检会跳过，避免误重启。 */
/* ------------------------------------------------------------------ */
#define OTA_CHUNK       1024
#define OTA_TIMEOUT_MS  60000

static void ota_resp(httpd_req_t *req, const char *status_str, const char *body)
{
    resp_status(req, status_str);
    resp_type(req, "text/plain; charset=utf-8");
    resp_hdr(req, "Access-Control-Allow-Origin", "*");
    resp_hdr(req, "Connection", "close");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* 跨域（双击本地文件打开）时，浏览器会先发 OPTIONS 预检，必须回 204 + CORS */
static esp_err_t h_update_opts(httpd_req_t *req)
{
    resp_hdr(req, "Access-Control-Allow-Origin", "*");
    resp_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    resp_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    resp_hdr(req, "Access-Control-Max-Age", "3600");
    resp_status(req, "204 No Content");
    resp_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_update(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        ota_resp(req, "400 Bad Request", "缺少固件长度（Content-Length）");
        return ESP_OK;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ota_resp(req, "500 Internal Server Error", "找不到空闲的 OTA 分区");
        return ESP_OK;
    }
    if ((size_t)req->content_len > part->size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "固件 %d 字节超过分区容量 %u 字节",
                 req->content_len, (unsigned)part->size);
        ota_resp(req, "413 Payload Too Large", msg);
        return ESP_OK;
    }

    esp_ota_handle_t ohandle = 0;
    esp_err_t ret = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ohandle);
    if (ret != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "OTA 初始化失败: %s", esp_err_to_name(ret));
        ota_resp(req, "500 Internal Server Error", msg);
        return ESP_OK;
    }

    s_ota_in_progress = true;
    ESP_LOGI(TAG, "OTA 开始：固件 %d 字节 → 分区 %s", req->content_len, part->label);

    uint8_t *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        esp_ota_abort(ohandle);
        s_ota_in_progress = false;
        ota_resp(req, "500 Internal Server Error", "内存不足");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int written   = 0;
    int timeouts  = 0;
    esp_err_t result = ESP_OK;
    uint32_t t0 = xTaskGetTickCount();

    while (remaining > 0) {
        int to_read = (remaining > OTA_CHUNK) ? OTA_CHUNK : remaining;
        int got = httpd_req_recv(req, (char *)buf, to_read);
        if (got == 0) {
            break;                       /* 连接关闭 / 体已读完 */
        }
        if (got < 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > 30) { result = ESP_ERR_TIMEOUT; break; }
                continue;                /* 超时重试 */
            }
            result = ESP_FAIL;
            break;
        }
        timeouts = 0;
        ret = esp_ota_write(ohandle, buf, (size_t)got);
        if (ret != ESP_OK) { result = ret; break; }
        remaining -= got;
        written   += got;
        if ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS > OTA_TIMEOUT_MS) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }
    free(buf);

    if (result == ESP_OK) {
        ret = esp_ota_end(ohandle);
        if (ret != ESP_OK) {
            result = ret;
            ESP_LOGE(TAG, "OTA 校验失败: %s（固件不完整或损坏，已放弃）",
                     esp_err_to_name(ret));
        }
    }
    if (result == ESP_OK) {
        ret = esp_ota_set_boot_partition(part);
        if (ret != ESP_OK) {
            result = ret;
            ESP_LOGE(TAG, "OTA 设置启动分区失败: %s", esp_err_to_name(ret));
        }
    }

    if (result != ESP_OK) {
        if (ohandle != 0) esp_ota_abort(ohandle);
        s_ota_in_progress = false;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "OTA 失败: %s（旧固件保留，未损坏）", esp_err_to_name(result));
        ota_resp(req, "500 Internal Server Error", msg);
        return ESP_OK;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "OTA 成功接收 %d 字节，马上重启进入新固件", written);
    ota_resp(req, "200 OK", msg);
    s_ota_in_progress = false;
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* CONFIG_LWIP_MAX_SOCKETS=10，留 1 个给 DNS/系统；默认 7 太紧。
       backlog 默认只有 5，手机 + 电脑并发排队时新连接会被直接 RST，
       表现就是「网页打不开」，这里一并放宽。 */
    cfg.max_open_sockets  = 9;
    cfg.backlog_conn      = 12;
    cfg.max_uri_handlers  = 12;
    cfg.stack_size        = 10240;   /* h_cmd 4KB 回显 + query_get 2×1KB 解码缓冲 */
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 5;

    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(e));
        return e;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",         .method = HTTP_GET, .handler = h_root   },
        { .uri = "/api/cmd",  .method = HTTP_GET, .handler = h_cmd    },
        { .uri = "/api/stop", .method = HTTP_GET, .handler = h_stop   },
        { .uri = "/api/ping", .method = HTTP_GET, .handler = h_ping   },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/log",  .method = HTTP_GET, .handler = h_log    },
        { .uri = "/api/net",   .method = HTTP_GET, .handler = h_net    },
        { .uri = "/api/wifi",  .method = HTTP_GET, .handler = h_wifi   },
        { .uri = "/api/guard", .method = HTTP_GET, .handler = h_guard  },
        { .uri = "/api/update", .method = HTTP_POST,    .handler = h_update      },
        { .uri = "/api/update", .method = HTTP_OPTIONS, .handler = h_update_opts },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        e = httpd_register_uri_handler(s_server, &uris[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", uris[i].uri, esp_err_to_name(e));
            return e;
        }
    }
    e = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "404 重定向注册失败: %s", esp_err_to_name(e));
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 网页服务自愈：httpd 是单线程，一旦被某条命令占住，外部只能看到
   「连接被重置」。独立任务定期从板子内部自检，连续失败就重启板子，
   避免每次都要人跑过去拔电源。httpd_stop() 会等 httpd 线程退出，
   线程卡死时它自己也会卡住，所以这里用 esp_restart()。                */
/* ------------------------------------------------------------------ */

#define GUARD_START_DELAY_MS 30000   /* 上电先给 Wi-Fi / DHCP 留时间 */
#define GUARD_POLL_MS         5000
#define GUARD_FAIL_LIMIT      60

/* 2026-11-14 二轮（长时间遥控后偶发断连）根治，三处一起改：
 *  ① 探针地址：原来连「板子自己的 STA IP」，APSTA 模式下这个包要绕到路由器
 *     再回来，实测【持续误判】（外部 /api/guard 明明回 alive），于是每 5s
 *     刷一条警告、每 5 分钟把 fails 攒到 60。CONFIG_LWIP_NETIF_LOOPBACK=y
 *     已开，改连 127.0.0.1 才是真正的「本机自检」，不再依赖 Wi-Fi 空口。
 *  ② BLE 连接期间【完全跳过】自检（以前只跳过重启、仍然每 5s 建一次 socket）。
 *     每次 socket 都会触发 2.4GHz 射频活动，与 BLE 抢空口；长时间累积的
 *     丢包正是手机 supervision timeout / status=8 的残留来源。
 *  ③ 【彻底取消 esp_restart()】：只要手机曾经连过又短暂掉开，旧逻辑会在
 *     那个空窗立刻重启整板，手机必然断。网页控制台卡死只记日志、不重启，
 *     用户手动断电即可恢复；比起误杀蓝牙连接，这个取舍明确划算。 */
/* ⭐ 2026-11-14 四轮坐实：「自检永远失败」其实是探针自己写错了。
 *   h_guard 回的是标准 HTTP 响应，光响应头就有 ~100 字节；旧代码只 recv
 *   一次、最多 64 字节，然后在这 64 字节里找 "alive" —— 正文 "alive" 根本
 *   还没读到。所以无论连 STA IP / 127.0.0.1 / AP IP，都必然判失败，
 *   这才是「每 5 秒刷一条自检失败」的源头（以前误以为是 APSTA 绕路）。
 *   现在循环读到出现 alive / 对端关闭 / 超时为止。 */
static bool guard_probe_addr(uint32_t addr_net_order)
{
    if (addr_net_order == 0) return false;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return false;

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(80);
    dst.sin_addr.s_addr = addr_net_order;   /* 已是网络字节序 */

    bool ok = false;
    static const char req[] =
        "GET /api/guard HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0 &&
        send(fd, req, sizeof(req) - 1, 0) == (int)(sizeof(req) - 1)) {
        char buf[512];
        size_t used = 0;
        while (used < sizeof(buf) - 1) {
            const int n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
            if (n <= 0) break;              /* 超时或对端关闭 */
            used += (size_t)n;
            buf[used] = '\0';
            if (strstr(buf, "alive") != NULL) { ok = true; break; }
        }
    }
    close(fd);
    return ok;
}

/* 依次试回环 → AP 地址 → STA 地址：不同配网状态下哪个能通不一样，
   只要有一个回了 alive 就认为 httpd 活着。 */
static bool guard_probe(void)
{
    if (guard_probe_addr(htonl(0x7f000001u))) return true;
    if (s_ap_ip_net  && guard_probe_addr(s_ap_ip_net))  return true;
    if (s_sta_up && s_sta_ip_net && guard_probe_addr(s_sta_ip_net)) return true;
    return false;
}

static void httpd_guard_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(GUARD_START_DELAY_MS));

    int fails = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GUARD_POLL_MS));
        if (s_ota_in_progress) {
            continue;                   /* OTA 上传中，跳过 httpd 健康检查 */
        }
        /* 手机正在 BLE 遥控：连探针都不发，避免 Wi-Fi/BLE 抢 2.4GHz 射频 */
        if (ble_ctl_is_connected()) {
            fails = 0;
            continue;
        }
        if (guard_probe()) {
            fails = 0;
            continue;
        }
        fails++;
        /* 每 60 次（约 5 分钟）才记一条，别把 16KB 日志缓冲刷爆 */
        if (fails >= GUARD_FAIL_LIMIT) {
            ESP_LOGW(TAG, "网页服务自检累计失败 %d 次（仅记录，不再自动重启）", fails);
        }
        if (fails >= GUARD_FAIL_LIMIT) {
            fails = 0;                  /* 不再 esp_restart()：重启必断蓝牙 */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 极简 DNS 应答器：所有 A 查询 → 热点自己的 IP                          */
/* ------------------------------------------------------------------ */

static uint32_t ap_ip_net_order(void)
{
    esp_netif_t *netif = (s_ap_netif != NULL)
        ? s_ap_netif : esp_netif_get_handle_from_ifkey(AP_NETIF_KEY);
    if (netif != NULL) {
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            return info.ip.addr;
        }
    }
    return FALLBACK_AP_IP;
}

static int dns_build_reply(const uint8_t *req, size_t req_len,
                           uint8_t *reply, size_t cap, uint32_t ap_ip)
{
    if (req_len < 12 || req_len + 16 > cap) {
        return 0;
    }
    memcpy(reply, req, req_len);

    if ((reply[2] & 0x78) != 0x00) return 0;              /* 非标准查询 */
    if (reply[4] != 0x00 || reply[5] != 0x01) return 0;   /* QDCOUNT != 1 */

    /* 跳过 question 的域名 labels，找到 type/class */
    size_t off = 12;
    while (off < req_len && reply[off] != 0) {
        if ((reply[off] & 0xC0) != 0) return 0;            /* 压缩指针，不处理 */
        off += 1u + reply[off];
        if (off > req_len) return 0;
    }
    if (off >= req_len || reply[off] != 0) return 0;
    const size_t qend = off + 1 + 4;
    if (qend > req_len) return 0;

    const uint16_t qtype  = (uint16_t)((reply[qend - 4] << 8) | reply[qend - 3]);
    const uint16_t qclass = (uint16_t)((reply[qend - 2] << 8) | reply[qend - 1]);
    if (qtype != 1 || qclass != 1) return 0;               /* 只答 A / IN */

    reply[2] |= 0x80;                                      /* QR = 1 */
    reply[3] |= 0x80;                                      /* RA = 1 */
    reply[6] = 0; reply[7] = 1;                            /* ANCOUNT = 1 */
    reply[8] = reply[9] = reply[10] = reply[11] = 0;       /* NSCOUNT / ARCOUNT = 0 */

    size_t a = req_len;
    reply[a++] = 0xC0; reply[a++] = 0x0C;                  /* 名字指针 → offset 12 */
    reply[a++] = 0x00; reply[a++] = 0x01;                  /* TYPE = A */
    reply[a++] = 0x00; reply[a++] = 0x01;                  /* CLASS = IN */
    reply[a++] = 0x00; reply[a++] = 0x00;
    reply[a++] = 0x00; reply[a++] = 0x3C;                  /* TTL = 60s */
    reply[a++] = 0x00; reply[a++] = 0x04;                  /* RDLENGTH = 4 */
    reply[a++] = (uint8_t)(ap_ip        & 0xFF);
    reply[a++] = (uint8_t)((ap_ip >>  8) & 0xFF);
    reply[a++] = (uint8_t)((ap_ip >> 16) & 0xFF);
    reply[a++] = (uint8_t)((ap_ip >> 24) & 0xFF);
    return (int)a;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = -1;

    for (;;) {
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGE(TAG, "DNS socket 创建失败 errno=%d，1s 后重试", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                ESP_LOGW(TAG, "DNS SO_RCVTIMEO 设置失败 errno=%d", errno);
            }
            /* 只绑 AP 侧地址：STA 连上家里路由后，绝不能让这个应答器
             * 干扰同一局域网里其它设备发往 ESP32 的 DNS 查询。 */
            struct sockaddr_in bind_addr = {
                .sin_family      = AF_INET,
                .sin_port        = htons(DNS_PORT),
                .sin_addr.s_addr = s_ap_ip_net,
            };
            if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
                ESP_LOGE(TAG, "DNS 端口 53 绑定失败 errno=%d", errno);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "DNS 应答器就绪（仅 AP 侧，所有 A 查询 → 热点 IP）");
        }

        uint8_t req[DNS_BUF];
        uint8_t reply[DNS_BUF + 16];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, req, sizeof(req), 0,
                               (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;                                  /* 1s 超时，正常轮询 */
            }
            ESP_LOGW(TAG, "DNS recvfrom 失败 errno=%d", errno);
            close(sock);
            sock = -1;
            continue;
        }

        const int rlen = dns_build_reply(req, (size_t)n, reply, sizeof(reply), s_ap_ip_net);
        if (rlen <= 0) {
            continue;
        }
        if (sendto(sock, reply, (size_t)rlen, 0,
                   (struct sockaddr *)&from, sizeof(from)) < 0) {
            ESP_LOGW(TAG, "DNS sendto 失败 errno=%d", errno);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Wi-Fi：AP 热点常驻 + 可选 STA 连家里路由（APSTA 共存）               */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_sta_count++;
        ESP_LOGI(TAG, "设备连上热点（当前 %d 台）", (int)s_sta_count);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_sta_count > 0) s_sta_count--;
        ESP_LOGI(TAG, "设备断开热点（当前 %d 台）", (int)s_sta_count);
    } else if (id == WIFI_EVENT_STA_START) {
        if (s_sta_configured) {
            const esp_err_t e = esp_wifi_connect();
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "发起连接失败: %s（热点仍可用）", esp_err_to_name(e));
            } else {
                ESP_LOGI(TAG, "正在连接家里 WiFi [%s]", s_sta_ssid);
            }
        }
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "已关联 [%s]，等待路由器分配 IP", s_sta_ssid);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        s_sta_ip_net = 0;
        s_gw_ip_net = 0;
        /* 家里路由失联：把兜底热点开回来（若刚才因 STA 成功被关掉）。
         * 只有真的在纯 STA 模式才切，避免每次重连失败都白折腾一遍。 */
        wifi_mode_t cur = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&cur) == ESP_OK && cur == WIFI_MODE_STA) {
            const esp_err_t em = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (em == ESP_OK) {
                s_ap_muted = false;
                ESP_LOGW(TAG, "家里路由失联，热点 spider8 已重新开启作兜底");
            } else {
                ESP_LOGW(TAG, "重开热点失败: %s（STA 仍会重试）", esp_err_to_name(em));
            }
        }
        if (s_sta_configured && s_sta_retry < STA_MAX_RETRY) {
            s_sta_retry++;
            ESP_LOGW(TAG, "家里 WiFi 断开，重连 %d/%d（热点仍可用）",
                     s_sta_retry, STA_MAX_RETRY);
            const esp_err_t e = esp_wifi_connect();
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "重连失败: %s", esp_err_to_name(e));
            }
        } else {
            ESP_LOGW(TAG, "家里 WiFi 不可用（密码错/信号弱），只保留热点；可在网页重新配网");
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP || data == NULL) {
        return;
    }
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    s_sta_ip_net = ev->ip_info.ip.addr;
    s_gw_ip_net  = ev->ip_info.gw.addr;
    s_sta_up = true;
    s_sta_retry = 0;

    /* 已从家里路由拿到 IP：关掉兜底热点，平时不占用 2.4G、列表里也看不到。
     * 断网（WIFI_EVENT_STA_DISCONNECTED）会自动开回来，不会失联。 */
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) == ESP_OK && cur == WIFI_MODE_APSTA) {
        const esp_err_t em = esp_wifi_set_mode(WIFI_MODE_STA);
        if (em == ESP_OK) {
            s_ap_muted = true;
            ESP_LOGI(TAG, "已连上家里路由，热点 spider8 自动关闭（断网会自动重开）");
        } else {
            ESP_LOGW(TAG, "关热点失败: %s（保持 APSTA，不影响使用）", esp_err_to_name(em));
        }
    }

    char ip[16];
    ip_str(s_sta_ip_net, ip, sizeof(ip));
    ESP_LOGI(TAG, "已连上家里路由器，电脑照常上网；控制台 http://%s", ip);
    ESP_LOGI(TAG, "也可试 http://%s（若路由器支持按主机名解析）", STA_HOSTNAME);
}

/* sta_pass 只在初始化栈上存在，不落到全局，避免密码长期驻留内存。 */
static esp_err_t wifi_init_network(const char *sta_pass)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&init_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(e));
        return e;
    }

    e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 事件注册失败: %s", esp_err_to_name(e));
        return e;
    }
    e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &ip_event_handler, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "IP 事件注册失败: %s", esp_err_to_name(e));
        return e;
    }

    /* 顺序铁律：必须先 set_mode 再 set_config —— esp_wifi_init 后各接口
     * 还不存在，先配 STA 会报 ESP_ERR_WIFI_MODE（本次实测踩到）。 */
    wifi_mode_t mode = WIFI_MODE_AP;
    if (s_sta_configured && sta_pass != NULL) {
        mode = WIFI_MODE_APSTA;
    }
    e = esp_wifi_set_mode(mode);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "设置 Wi-Fi 模式失败: %s", esp_err_to_name(e));
        return e;
    }

    if (mode == WIFI_MODE_APSTA) {
        wifi_config_t sta_cfg;
        memset(&sta_cfg, 0, sizeof(sta_cfg));
        /* 不用 snprintf：源缓冲比目标多 1 字节，GCC 会按
         * -Werror=format-truncation 判为可能截断。net_cred_valid() 已保证
         * SSID ≤32、密码 ≤63，这里按实际长度精确拷贝；结构体已清零，
         * 未满时自动以 '\0' 结尾，满 32 字节的 SSID 也合法。 */
        const size_t nssid = strlen(s_sta_ssid);
        memcpy(sta_cfg.sta.ssid, s_sta_ssid,
               (nssid < sizeof(sta_cfg.sta.ssid)) ? nssid : sizeof(sta_cfg.sta.ssid));
        const size_t npass = strlen(sta_pass);
        memcpy(sta_cfg.sta.password, sta_pass,
               (npass < sizeof(sta_cfg.sta.password)) ? npass : sizeof(sta_cfg.sta.password));
        /* 其余字段全 0：不限制认证方式、不做 BSSID 锁定、用路由器信道 */
        e = esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "STA 配置无效，降级为仅热点: %s", esp_err_to_name(e));
            s_sta_configured = false;
            e = esp_wifi_set_mode(WIFI_MODE_AP);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "回退纯 AP 模式失败: %s", esp_err_to_name(e));
                return e;
            }
        }
    }

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
             "%s", CONFIG_SPIDER8_WIFI_SSID);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel  = (uint8_t)CONFIG_SPIDER8_WIFI_CHANNEL;
    ap_cfg.ap.max_connection = (uint8_t)CONFIG_SPIDER8_WIFI_MAX_CONN;

#ifdef CONFIG_SPIDER8_WIFI_OPEN
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
#else
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password),
             "%s", CONFIG_SPIDER8_WIFI_PASS);
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
#endif

    e = esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "热点参数无效（密码需 8~63 位）: %s", esp_err_to_name(e));
        return e;
    }
    e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(e));
        return e;
    }

    s_ap_ip_net = ap_ip_net_order();
    char ap_ip[16];
    ip_str(s_ap_ip_net, ap_ip, sizeof(ap_ip));
    ESP_LOGI(TAG, "热点 %s 已启动，控制台 http://%s（模式 %s）",
             (char *)ap_cfg.ap.ssid, ap_ip,
             (mode == WIFI_MODE_APSTA) ? "AP+STA" : "仅 AP");
#ifdef CONFIG_SPIDER8_WIFI_OPEN
    ESP_LOGW(TAG, "当前是开放热点，附近任何人都能连上并控制机器狗");
#else
    ESP_LOGI(TAG, "热点密码: %s", CONFIG_SPIDER8_WIFI_PASS);
#endif
    if (mode == WIFI_MODE_APSTA) {
        ESP_LOGW(TAG, "APSTA 下热点信道会跟随路由器，属正常现象");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 断连急停看门狗                                                      */
/* ------------------------------------------------------------------ */

static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WD_POLL_MS));
        if (!s_wd_armed) {
            continue;
        }
        const uint32_t dt = (xTaskGetTickCount() - s_last_ping_tick) * portTICK_PERIOD_MS;
        if (dt <= WD_TIMEOUT_MS) {
            continue;
        }
        s_wd_armed = false;             /* 只触发一次，之后有线操作不受影响 */
        if (motion_mode() == MODE_WALK) {
            motion_stand();
            ESP_LOGW(TAG, "无线心跳丢失 %lums，自动停车站定", (unsigned long)dt);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

esp_err_t wireless_init(void)
{
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(e));
        return e;
    }

    /* IDF 5.5 没有「查询默认循环是否存在」的 API：重复创建会返回
     * ESP_ERR_INVALID_STATE，视为「已经有人建好了」即可。 */
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "默认事件循环创建失败: %s", esp_err_to_name(e));
        return e;
    }

    /* 配网决定开机模式：有家里 WiFi 配置就 APSTA，没有就纯 AP。
     * 读失败不算错误 —— 没配过网时 NVS 命名空间本来就不存在。 */
    char ssid[WIFI_SSID_MAX] = { 0 };
    char pass[WIFI_PASS_MAX] = { 0 };
    s_sta_configured = false;
    s_sta_ssid[0] = '\0';
    if (net_cfg_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK &&
        net_cred_valid(ssid, pass)) {
        s_sta_configured = true;
        snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
        ESP_LOGI(TAG, "NVS 读到家里 WiFi 配置: [%s]", s_sta_ssid);
    } else {
        ESP_LOGI(TAG, "未配置家里 WiFi，仅启动热点（网页里可随时配网）");
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "SoftAP 网络接口创建失败");
        return ESP_FAIL;
    }

    if (s_sta_configured) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            /* 降级：热点仍然要起来，否则无线链路整条废掉 */
            ESP_LOGE(TAG, "STA 网络接口创建失败，降级为仅热点");
            s_sta_configured = false;
            s_sta_ssid[0] = '\0';
        } else {
            e = esp_netif_set_hostname(s_sta_netif, STA_HOSTNAME);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "DHCP 主机名设置失败（不影响连接）: %s",
                         esp_err_to_name(e));
            }
        }
    }

    e = wifi_init_network(pass);
    if (e != ESP_OK) {
        return e;
    }

    /* 强制门户会产生大量探测请求，压低 httpd 自身日志避免刷屏 */
    esp_log_level_set("httpd_uri",   ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx",  ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("httpd_sess",  ESP_LOG_ERROR);

    e = start_webserver();
    if (e != ESP_OK) {
        return e;
    }

    if (xTaskCreate(dns_task, "dns53", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "DNS 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(watchdog_task, "wd_wifi", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "无线看门狗任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(httpd_guard_task, "wd_http", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "网页服务自愈任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "注意：Wi-Fi 发射瞬间会多耗约 0.3A，请确认 5V 电源带载不塌");
    ESP_LOGW(TAG, "APSTA 双网口同时发射，峰值电流比纯热点更高，务必确认舵机 5V 不掉压");
    ESP_LOGI(TAG, "无线测试台就绪；有线 USB 串口仍然可用");
    if (!s_sta_configured) {
        ESP_LOGI(TAG, "想边上网边控狗：在网页「网络与配网」里填一次家里 WiFi 即可");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 有线诊断：热点搜不到时插 USB 线敲 `wifidiag`                        */
/* ------------------------------------------------------------------ */

void wireless_diag(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t e = esp_wifi_get_mode(&mode);
    cli_out("[wifidiag] Wi-Fi 模式: %s, mode=%d (0=未起 1=STA 2=AP 3=APSTA)\r\n",
            esp_err_to_name(e), (int)mode);

    char ap_ip[16], sta_ip[16], gw_ip[16];
    ip_str(s_ap_ip_net, ap_ip, sizeof(ap_ip));
    ip_str(s_sta_ip_net, sta_ip, sizeof(sta_ip));
    ip_str(s_gw_ip_net, gw_ip, sizeof(gw_ip));

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    e = esp_wifi_get_config(ESP_IF_WIFI_AP, &cfg);
    cli_out("[wifidiag] AP 热点: %s | SSID=[%s] 认证=%d 上限=%d IP=%s | 状态=%s\r\n",
            esp_err_to_name(e), (char *)cfg.ap.ssid,
            (int)cfg.ap.authmode, (int)cfg.ap.max_connection, ap_ip,
            (mode == WIFI_MODE_STA) ? "已自动关闭（STA在线，断网自动重开）" : "开启");

    cli_out("[wifidiag] 家里WiFi配网=%s SSID=[%s] | STA已拿到IP=%s 重连次数=%d\r\n",
            s_sta_configured ? "已配置" : "未配置",
            s_sta_configured ? s_sta_ssid : "(无)",
            s_sta_up ? "是" : "否", s_sta_retry);
    cli_out("[wifidiag] STA 地址=%s 网关=%s DHCP主机名=%s（mDNS 未启用，别加 .local）\r\n",
            s_sta_up ? sta_ip : "(未分配)", s_sta_up ? gw_ip : "-", STA_HOSTNAME);

    if (s_sta_up) {
        wifi_ap_record_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        e = esp_wifi_sta_get_ap_info(&ap_info);
        cli_out("[wifidiag] 路由器信号: %s RSSI=%d dBm 信道=%d\r\n",
                esp_err_to_name(e), (int)ap_info.rssi, (int)ap_info.primary);
        cli_out("[wifidiag] >>> 连家里WiFi时用这个地址: http://%s （路由器支持时可用 http://%s）\r\n",
                sta_ip, STA_HOSTNAME);
    } else {
        cli_out("[wifidiag] >>> 当前请连热点 spider8，用 http://%s\r\n", ap_ip);
    }

    wifi_sta_list_t sta;
    memset(&sta, 0, sizeof(sta));
    e = esp_wifi_ap_get_sta_list(&sta);
    cli_out("[wifidiag] 热点上已连设备: %d (%s)\r\n",
            (e == ESP_OK) ? (int)sta.num : -1, esp_err_to_name(e));

    cli_out("[wifidiag] HTTP服务=%s | 看门狗armed=%d | 配网接口 /api/wifi 可用\r\n",
            (s_server != NULL) ? "已启动" : "未启动", (int)s_wd_armed);
}

esp_err_t wireless_clear_sta_config(void)
{
    const esp_err_t e = net_cfg_clear();
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "已清除家里 WiFi 配网；断电重启后回到纯热点模式");
    } else {
        ESP_LOGE(TAG, "清除家里 WiFi 配网失败: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t wireless_set_sta_config(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL) {
        cli_out("用法: wifiset <ssid> <pass>（开放网络: wifiset <ssid> -）\r\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(pass, "-") == 0) {
        pass = "";
    }
    if (!net_cred_valid(ssid, pass)) {
        cli_out("参数不合法：SSID 需 1~32 字符；密码 8~63 字符或用 - 表示开放网络\r\n");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t e = net_cfg_save(ssid, pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "配网写入 NVS 失败: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGW(TAG, "已写入家里 WiFi [%s]，1.5 秒后重启生效（重启不擦舵机 trim）",
             ssid);
    const esp_err_t er = schedule_restart();
    if (er != ESP_OK) {
        cli_out("已保存但无法自动重启，请手动断电重启\r\n");
        return er;
    }
    cli_out("已保存 [%s]，板子 1.5 秒后重启；重启后热点只在连不上路由时才会开启\r\n",
            ssid);
    return ESP_OK;
}

#else  /* !CONFIG_SPIDER8_WIFI_ENABLED */

esp_err_t wireless_init(void)
{
    printf("wireless: 无线测试台未启用（menuconfig: Spider8 无线测试台）\r\n");
    return ESP_OK;
}

void wireless_diag(void)
{
    printf("[wifidiag] 编译时未启用 CONFIG_SPIDER8_WIFI_ENABLED\r\n");
}

esp_err_t wireless_clear_sta_config(void)
{
    printf("[wificlear] 编译时未启用 CONFIG_SPIDER8_WIFI_ENABLED\r\n");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wireless_set_sta_config(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;
    printf("[wifiset] 编译时未启用 CONFIG_SPIDER8_WIFI_ENABLED\r\n");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_SPIDER8_WIFI_ENABLED */
