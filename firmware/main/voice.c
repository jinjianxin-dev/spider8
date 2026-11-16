/*
 * Spider8 语音前端 —— I2：识别结果经安全门后交给 cli_exec() 驱动动作。
 *
 * 从 E:\esp32-tools\projects\voice-llm\main\main.c 剥出音频采集 / AFE /
 * 唤醒词 / 命令词四级流水线，去掉了点灯小模型(llm_runtime)与 GPIO 执行，
 * 换成一张 25 条命令词表和一条「最新覆盖」队列。
 *
 * 【I2 的四道安全门】
 *  1) 白名单：只有 k_cmds[] 里登记过的 id / CLI 文本能执行，表外一律拒绝。
 *     命令文本是编译期常量，不含任何用户输入的字符串，天然限幅。
 *  2) 运行时放行开关 s_exec，默认 **1（放行）**（2026-11-16 用户拍板：喇叭与
 *     动作均已验收，要求重启后免手动开闸）。`voice exec off` 可临时关闸，
 *     关闸瞬间立即回站姿；⚠️ 上电即放行，把狗放地上前请先关闸。
 *  3) 急停直通：停/停下/别动/停止 直接 motion_stand()，不排 cli_exec 的
 *     2.5s 执行锁；任何时候（含 exec=off）喊「停」都有效。
 *  4) 5s 行走超时：walk/trot/left/right/straight/demo 触发后自动站定，
 *     避免「走起来停不下来」；新命令会重置计时。
 *
 * 任务全部绑 core0；伺服任务在 motion.c 里绑 core1，两边互不抢 20ms 节拍。
 * voice_cmd_task 可能被 cli_exec 阻塞 2.5s，因此它绝不做音频采集/识别。
 *
 * 引脚：INMP441 I2S0 RX = BCLK5 / WS6 / DIN7；MAX98357 I2S1 TX = 15/16/18
 *       （喇叭在 mouth.c）。PCA9685 只占 8/9，不冲突。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "model_path.h"

#include "cli_out.h"
#include "motion.h"
#include "mouth.h"
#include "voice_clips.h"
#include "voice.h"

static const char *TAG = "voice";

#define PIN_MIC_BCLK   GPIO_NUM_5
#define PIN_MIC_WS     GPIO_NUM_6
#define PIN_MIC_DIN    GPIO_NUM_7
#define SAMPLE_RATE    16000

#define VOICE_CORE     0          /* 语音一律 core0；单核配置下也是 0，安全 */
#define FEED_PRIO      5
#define DETECT_PRIO    5
#define CMD_PRIO       4
#define FEED_STACK     (8 * 1024)
#define DETECT_STACK   (8 * 1024)
#define CMD_STACK      (4 * 1024)

/* ------------------------------------------------------------------ */
/* 一期命令词表（docs/16 §3 定稿）                                      */
/* py 必须是【无声调拼音串】：MultiNet7 不接受汉字，也不接受阿拉伯数字。
 * cli 是 I2 真正交给 cli_exec() 的命令文本，全部为编译期常量白名单。
 *   "STOP"         = 走 motion_stand() 急停直通
 *   "UNSUPPORTED"  = 识别到但明确拒绝（当前固件没有这个动作）
 */
/* ------------------------------------------------------------------ */
typedef struct {
    int         id;
    const char *cn;      /* 中文说法，只用于日志 */
    const char *py;      /* 注册给 MultiNet 的拼音 */
    const char *cli;     /* I2 映射到的 CLI 命令 */
} voice_cmd_t;

static const voice_cmd_t k_cmds[] = {
    { 1,  "前进",   "qian jin",       "walk 50"       },
    /* 后退：方案原写 "walk -50"，实测无效 —— execute_pending() 里
     * `if (s_req_speed >= 0) motion_set_speed(...)`，且 motion_set_speed()
     * 把负值夹成 0，负速度会退化成「按当前速度前进」，是危险的错误动作。
     * 现有步态层没有后退能力，I2 明确拒绝，留到单独一期做。 */
    { 2,  "后退",   "hou tui",        "UNSUPPORTED"   },
    { 3,  "慢走",   "man zou",        "walk 30"       },
    { 4,  "快走",   "kuai zou",       "walk 80"       },
    { 5,  "快跑",   "kuai pao",       "trot 70"       },
    { 6,  "左转",   "zuo zhuan",      "left"          },
    { 7,  "右转",   "you zhuan",      "right"         },
    { 8,  "转个圈", "zhuan ge quan",  "left"          },
    { 9,  "直走",   "zhi zou",        "straight"      },
    { 10, "停止",   "ting zhi",       "STOP"          },
    { 11, "停下",   "ting xia",       "STOP"          },
    { 12, "别动",   "bie dong",       "STOP"          },
    { 13, "停",     "ting",           "STOP"          }, /* 单音节急停别名，I2 口测误触发率 */
    /* 命令表里没有独立的 `stand` 命令，站立是 `pose stand`（cmd_pose）。
     * 照原方案写 "stand" 会得到「未知命令」，识别成功却什么也不做。 */
    { 14, "站好",   "zhan hao",       "pose stand"    },
    { 15, "站起来", "zhan qi lai",    "pose stand"    },
    { 16, "蹲下",   "dun xia",        "pose crouch"   },
    { 17, "趴下",   "pa xia",         "pose lie"      },
    { 18, "握手",   "wo shou",        "pose shake"    },
    { 19, "挥手",   "hui shou",       "pose wave"     },
    { 20, "演示",   "yan shi",        "demo"          },
    { 21, "快一点", "kuai yi dian",   "speed 80"      },
    { 22, "慢一点", "man yi dian",    "speed 35"      },
    { 23, "大步",   "da bu",          "stride 24"     },
    { 24, "小步",   "xiao bu",        "stride 14"     },
    { 25, "查内存", "cha nei cun",    "meminfo"       },
};
#define N_CMDS ((int)(sizeof(k_cmds) / sizeof(k_cmds[0])))

static const voice_cmd_t *find_cmd(int id)
{
    for (int i = 0; i < N_CMDS; i++) {
        if (k_cmds[i].id == id) return &k_cmds[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    int  id;        /* >0 命令 id；0 = 唤醒事件 */
    bool wake;
    bool stopped;   /* 急停已在 detect 任务直通执行，cmd 任务别重复动作 */
} voice_evt_t;

/* I2 急停直通（定义在 detect_task 之后，这里先声明给 detect 用）。
 * 急停必须零排队：cmd 任务可能正被 cli_exec 的执行锁堵住最多 2.5s。 */
static void stop_now(const char *why);

static QueueHandle_t  s_q = NULL;
static TaskHandle_t   s_h_feed = NULL, s_h_detect = NULL, s_h_cmd = NULL;
static volatile int   s_running = 0;
static volatile int   s_enabled = 1;
static volatile int   s_ready = 0;
static volatile int   s_mic_peak = 0;
static volatile int   s_hits = 0;
static volatile int   s_wakes = 0;
/* I2S 32bit 帧 -> 16bit PCM 的右移量。原 voice-llm 用 14（放大 4 倍），
 * 本机麦克风灵敏度高，口测时说话峰值多次顶到 32768（削顶失真），唤醒词
 * 还能勉强命中、命令词全灭。默认改 16（不放大），可用 voice gain 现场调。 */
static volatile int   s_mic_shift = 16;

/* ---- I2 安全门状态 ------------------------------------------------ */
#define VOICE_MOVE_TIMEOUT_US   (5 * 1000 * 1000LL)  /* 行走 5s 自动站定 */
static volatile int   s_exec = 1;          /* 上电默认放行（11-16 用户拍板）；voice exec off 可临时关闸 */
static volatile int   s_muted = 0;         /* 喇叭已修好，上电默认播报；voice mute on 可临时静音 */
static volatile int   s_exec_count = 0;    /* 实际执行过的命令数 */
static volatile int   s_stop_count = 0;    /* 急停次数（含超时自动停） */
static volatile int64_t s_move_deadline_us = 0;  /* 0 = 没有在计时 */
static char s_exec_out[CLI_OUT_MAX];       /* cli_exec 回话，放 .bss 不占任务栈 */

static i2s_chan_handle_t       s_rx = NULL;
static srmodel_list_t         *s_models = NULL;
static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t      *s_afe_data = NULL;
static esp_mn_iface_t         *s_mn = NULL;
static model_iface_data_t     *s_mn_data = NULL;

bool voice_is_ready(void) { return s_ready != 0; }

static void mem_snapshot(const char *stage)
{
    ESP_LOGI(TAG, "[MEM] %-16s 片内free=%lu min=%lu 最大块=%lu | PSRAMfree=%lu",
             stage,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ------------------------------------------------------------------ */
/* I2S 麦克风（INMP441，32bit 左声道，抽成 16kHz / 16bit mono）          */
/* ------------------------------------------------------------------ */
static esp_err_t i2s_mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_MIC_DIN,
        },
    };
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_rx);
}

/* ------------------------------------------------------------------ */
/* feed：I2S -> AFE。只搬运，绝不做识别，也不阻塞在别处。                */
/* 音频大缓冲放 PSRAM，DMA 缓冲放片内 —— 这是单板能共存的关键分配方式。  */
/* ------------------------------------------------------------------ */
static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int nch   = s_afe->get_feed_channel_num(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t) * nch, MALLOC_CAP_SPIRAM);
    int32_t *raw = heap_caps_malloc(2 * chunk * sizeof(int32_t), MALLOC_CAP_DMA);
    if (!buf || !raw) {
        ESP_LOGE(TAG, "feed 缓冲申请失败 chunk=%d nch=%d", chunk, nch);
        free(buf); free(raw);
        vTaskDelete(NULL);
        return;
    }

    int chunks = 0, peak = 0;
    long clipped = 0, samples = 0;
    while (s_running) {
        size_t br = 0;
        i2s_channel_read(s_rx, raw, 2 * chunk * sizeof(int32_t), &br, portMAX_DELAY);
        int got = br / sizeof(int32_t) / 2;      /* S3 std RX 给的是 stereo 帧，取偶数槽 */
        if (got > chunk) got = chunk;
        for (int i = 0; i < got; i++) {
            int32_t v = raw[2 * i] >> s_mic_shift;
            if (v > 32767 || v < -32768) clipped++;   /* 削顶样本计数 */
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            buf[i] = (int16_t)v;
        }
        samples += got;
        for (int i = got; i < chunk; i++) buf[i] = 0;
        s_afe->feed(s_afe_data, buf);

        /* 每约 5 秒报一次麦克风峰值与削顶比例：peak=0 说明没采到声音；
         * clip>0 说明输入过载失真，命令词识别会明显变差，需调 voice gain。 */
        if (++chunks >= 150) {
            s_mic_peak = peak;
            ESP_LOGI(TAG, "[mic] peak=%d clip=%.2f%% gain=%d (0=静音/未接好)",
                     peak, samples ? (100.0 * clipped / samples) : 0.0, s_mic_shift);
            chunks = 0; peak = 0; clipped = 0; samples = 0;
        }
    }
    free(buf); free(raw);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* detect：AFE -> 唤醒词 -> 命令词，命中只投队列（0 超时）。             */
/* 绝不能在这里调 cli_exec()：它可能等 2.5s 执行锁，会堵死音频流。       */
/* ------------------------------------------------------------------ */
static void detect_task(void *arg)
{
    (void)arg;
    int wakeup = 0, ear_gated = 0, last_vad = -1, last_wn = -1;

    ESP_LOGI(TAG, "开始聆听：先说「你好小智」，再说命令词；动作放行 exec=%d（%s，上电默认放行）",
             s_exec, s_exec ? "会动腿" : "只记录");
    while (s_running) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch 失败，语音前端停止");
            break;
        }

        /* 半双工：喇叭说话时把耳朵关掉，否则自己播的「好的」会被自己识别，
         * 形成无限循环。播完再多闭 300ms 让房间回声散掉（mouth.c 里实现）。 */
        int gated = mouth_gate_closed() ? 1 : 0;
        if (gated != ear_gated) {
            ear_gated = gated;
            s_mn->clean(s_mn_data);
            ESP_LOGI(TAG, "[ear] %s", gated ? "闭麦（喇叭播报中，识别暂停）" : "开麦");
        }
        if (gated) continue;

        /* 关掉识别时仍要持续 fetch  draining AFE，不能 delay，否则音频环缓冲溢出 */
        if (!s_enabled) { wakeup = 0; continue; }

        if ((int)res->vad_state != last_vad) {
            last_vad = (int)res->vad_state;
            ESP_LOGI(TAG, "[vad] %s vol=%.1f dBFS",
                     (int)res->vad_state == (int)AFE_VAD_SPEECH ? "SPEECH" : "silence",
                     res->data_volume);
        }
        if ((int)res->wakeup_state != last_wn) {
            last_wn = (int)res->wakeup_state;
            if (last_wn != (int)WAKENET_NO_DETECT)
                ESP_LOGI(TAG, "[wn] state=%d", last_wn);
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, ">> 唤醒词命中「你好小智」");
            s_mn->clean(s_mn_data);
            wakeup = 1;
            s_wakes++;
            voice_evt_t ev = { .id = 0, .wake = true };
            xQueueOverwrite(s_q, &ev);
            continue;
        }
        if (!wakeup) continue;

        esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
        if (st == ESP_MN_STATE_DETECTING) continue;

        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            int id = (r && r->num > 0) ? r->command_id[0] : 0;
            voice_evt_t ev = { .id = id, .wake = false };
            /* 急停直通：不等队列、不等 cli_exec 的 2.5s 执行锁。
             * motion_stand() 只设置目标状态，微秒级返回，不会堵住音频流。 */
            const voice_cmd_t *hit = find_cmd(id);
            if (hit && strcmp(hit->cli, "STOP") == 0) {
                stop_now(hit->cn);
                ev.stopped = true;
            }
            xQueueOverwrite(s_q, &ev);       /* 队列长 1：只保留最新一条 */
            s_mn->clean(s_mn_data);          /* 连续模式，不必重新唤醒 */
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, ">> 没听到命令，回到休眠");
            s_afe->enable_wakenet(s_afe_data);
            wakeup = 0;
        }
    }
    s_running = 0;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* I2 安全门：白名单 / 急停直通 / 5s 行走超时                          */
/* ------------------------------------------------------------------ */

/* 这些命令会让狗持续运动，必须挂 5s 自动停；pose / speed / stride / meminfo
 * 之类自己会回站姿或只改参数，不需要计时。 */
static bool is_moving_cli(const char *cli)
{
    return strncmp(cli, "walk",     4) == 0 ||
           strncmp(cli, "trot",     4) == 0 ||
           strncmp(cli, "left",     4) == 0 ||
           strncmp(cli, "right",    5) == 0 ||
           strncmp(cli, "straight", 8) == 0 ||
           strncmp(cli, "demo",     4) == 0;
}

static void say_if_unmuted(voice_clip_t id)
{
    if (!s_muted) mouth_say(id);
}

/* 急停动作本体：不排队、不等命令锁。detect 任务和 cmd 任务都能调。
 * motion_stand() 只设置目标状态，由 core1 的 motion_task 下一拍执行。 */
static void stop_now(const char *why)
{
    s_move_deadline_us = 0;
    motion_stand();
    ESP_LOGW(TAG, "[voice->STOP] %s：立即回站姿（零排队，未走命令锁）", why);
}

/* 完整急停（动作 + 计数 + 播报）：给 5s 超时保护和 voice sim 用。 */
static void do_emergency_stop(const char *why)
{
    stop_now(why);
    s_stop_count++;
    say_if_unmuted(VC_STOPPED);
}

static void exec_voice_cmd(const voice_cmd_t *c, bool already_stopped)
{
    ESP_LOGI(TAG, ">> heard: %s (id=%d 中文=%s -> CLI「%s」) exec=%d",
             c->py, c->id, c->cn, c->cli, s_exec);

    if (strcmp(c->cli, "STOP") == 0) {
        if (already_stopped) {
            s_stop_count++;              /* detect 已直通动作，这里只补计数/播报 */
            ESP_LOGI(TAG, "[voice] 急停已在识别任务直通完成（零排队）");
            say_if_unmuted(VC_STOPPED);
        } else {
            do_emergency_stop(c->cn);  /* voice sim 投送的急停走完整路径 */
        }
        return;
    }
    if (strcmp(c->cli, "UNSUPPORTED") == 0) {
        ESP_LOGW(TAG, "[voice] 「%s」当前固件不支持（%s），拒绝执行", c->cn, c->py);
        say_if_unmuted(VC_NOPE);
        return;
    }

    if (!s_exec) {
        ESP_LOGI(TAG, "[voice] 【只记录：exec=off】不执行动作；放行用 voice exec on");
        say_if_unmuted(VC_OK);
        return;
    }

    /* pose 等命令不写回话，先清掉上一次的残留，
     * 否则 BLE / App 上会看到上一条命令的旧文字（rc=0 也会串台）。 */
    s_exec_out[0] = '\0';
    int rc = cli_exec(c->cli, s_exec_out, sizeof(s_exec_out));
    s_exec_count++;
    char head[121];                       /* 回话可能很长，截短且压掉换行 */
    snprintf(head, sizeof(head), "%.120s", s_exec_out);
    for (char *p = head; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    ESP_LOGI(TAG, "[voice->cli] \"%s\" rc=%d 回话=%s", c->cli, rc, head);

    if (is_moving_cli(c->cli)) {
        s_move_deadline_us = esp_timer_get_time() + VOICE_MOVE_TIMEOUT_US;
        ESP_LOGI(TAG, "[voice] 5s 行走超时保护已启动/续期");
    } else if (strncmp(c->cli, "pose ", 5) == 0) {
        s_move_deadline_us = 0;           /* 姿态类自己会停，撤掉计时 */
    }
    /* 参数类（speed/stride/meminfo）不改 deadline：
     * 否则走路时喊一句「快一点」就会把 5s 自动停悄悄取消，狗会一直走。 */
    say_if_unmuted(VC_OK);
}

/* ------------------------------------------------------------------ */
/* cmd：出队 -> 日志 + 安全门 + 动作。cmd_task 会被 cli_exec 阻塞，      */
/*       所以音频采集/识别一律不放在这里。                              */
/* ------------------------------------------------------------------ */
static void voice_cmd_task(void *arg)
{
    (void)arg;
    voice_evt_t ev;
    while (s_running) {
        /* 5s 行走超时：cmd_task 空闲时每 100ms 查一次。若正阻塞在
         * cli_exec 里，最多晚 2.5s 生效（执行锁上限），仍远好于停不下来。 */
        if (s_move_deadline_us && esp_timer_get_time() >= s_move_deadline_us) {
            do_emergency_stop("5s 行走超时");
        }
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        if (ev.wake) {
            say_if_unmuted(VC_HERE);        /* 「我在」 */
            continue;
        }

        const voice_cmd_t *c = find_cmd(ev.id);
        if (!c) {
            ESP_LOGW(TAG, ">> 表外命令 id=%d（白名单拒绝，不执行）", ev.id);
            say_if_unmuted(VC_NOPE);
            continue;
        }
        s_hits++;
        exec_voice_cmd(c, ev.stopped);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* console 命令：voice [status|list|on|off|sim <id>|say <clip>]         */
/* ------------------------------------------------------------------ */
static void print_status(void)
{
    mem_snapshot("voice 当前");
    ESP_LOGI(TAG, "语音状态 ready=%d enabled=%d 唤醒=%d 命中=%d 麦克风peak=%d 增益shift=%d 闭麦=%d",
             s_ready, s_enabled, s_wakes, s_hits, s_mic_peak, s_mic_shift,
             (int)mouth_gate_closed());
    int64_t left_us = s_move_deadline_us ? (s_move_deadline_us - esp_timer_get_time()) : 0;
    if (left_us < 0) left_us = 0;
    ESP_LOGI(TAG, "动作放行 exec=%d（%s）静音 mute=%d 已执行=%d 急停=%d 行走超时倒计时=%.1fs",
             s_exec, s_exec ? "会动腿" : "只记录", s_muted,
             s_exec_count, s_stop_count, (double)left_us / 1000000.0);
    char mstat[128];
    mouth_stats(mstat, sizeof(mstat));
    ESP_LOGI(TAG, "喇叭 %s（mute=%d；上电默认播报，voice mute on|off 可切换）", mstat, s_muted);
    if (s_h_feed)   ESP_LOGI(TAG, "栈水位(字) feed=%lu detect=%lu cmd=%lu",
                             (unsigned long)uxTaskGetStackHighWaterMark(s_h_feed),
                             (unsigned long)(s_h_detect ? uxTaskGetStackHighWaterMark(s_h_detect) : 0),
                             (unsigned long)(s_h_cmd ? uxTaskGetStackHighWaterMark(s_h_cmd) : 0));
    ESP_LOGI(TAG, "命令词共 %d 条（白名单）；急停直通 + 5s 行走超时已启用", N_CMDS);
    /* 上面全是 ESP_LOGI，BLE / 网页的 cli_exec 抓不到。补一行短回话，
     * 手机 App 发 `voice` 也能一眼看到放行开关的当前状态。 */
    cli_out("voice ready=%d exec=%d(%s) mute=%d 唤醒=%d 命中=%d 已执行=%d 急停=%d 超时=%.1fs\n",
            s_ready, s_exec, s_exec ? "会动腿" : "只记录", s_muted,
            s_wakes, s_hits, s_exec_count, s_stop_count, (double)left_us / 1000000.0);
}

int voice_cli(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    if (strcmp(sub, "list") == 0) {
        for (int i = 0; i < N_CMDS; i++) {
            ESP_LOGI(TAG, " %2d  %-8s %-18s -> %s",
                     k_cmds[i].id, k_cmds[i].cn, k_cmds[i].py, k_cmds[i].cli);
        }
        return 0;
    }
    if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        s_enabled = (strcmp(sub, "on") == 0) ? 1 : 0;
        ESP_LOGI(TAG, "语音识别已%s（音频流保持运行）", s_enabled ? "开启" : "关闭");
        return 0;
    }
    if (strcmp(sub, "exec") == 0) {
        if (argc < 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
            ESP_LOGW(TAG, "用法: voice exec on|off（当前 %s）", s_exec ? "on" : "off");
            return 1;
        }
        int want = (strcmp(argv[2], "on") == 0) ? 1 : 0;
        if (!want && s_exec) {
            s_move_deadline_us = 0;
            motion_stand();          /* 关闸瞬间停车，不留「还在走」的状态 */
            ESP_LOGW(TAG, "[voice] 关闸：已立即回站姿");
        }
        s_exec = want;
        ESP_LOGW(TAG, "=== 语音动作放行 %s ===%s",
                 want ? "ON（会动腿）" : "OFF（只记录）",
                 want ? " 请确认四脚悬空、周围无障碍" : "");
        cli_out("语音动作放行=%s（%s）\n", want ? "ON" : "OFF",
                want ? "会动腿，请确认四脚悬空" : "只记录，腿不会动");
        return 0;
    }
    if (strcmp(sub, "mute") == 0) {
        if (argc < 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
            ESP_LOGW(TAG, "用法: voice mute on|off（当前 %s）", s_muted ? "on" : "off");
            return 1;
        }
        s_muted = (strcmp(argv[2], "on") == 0) ? 1 : 0;
        ESP_LOGI(TAG, "喇叭播报已%s；静音时不闭麦，唤醒后接命令词更顺",
                 s_muted ? "静音（不闭麦）" : "恢复播报");
        cli_out("喇叭播报=%s（静音时不闭麦，口测更顺）\n", s_muted ? "静音" : "恢复");
        return 0;
    }
    if (strcmp(sub, "gain") == 0) {
        if (argc < 3) {
            ESP_LOGW(TAG, "用法: voice gain <12-20>；数字越大音量越小（当前 %d）", s_mic_shift);
            return 1;
        }
        int g = atoi(argv[2]);
        if (g < 12 || g > 20) {
            ESP_LOGW(TAG, "gain 需在 12~20（当前 %d）", s_mic_shift);
            return 1;
        }
        s_mic_shift = g;
        ESP_LOGI(TAG, "麦克风增益 shift=%d（原工程 14；数字每 +1 音量减半）", g);
        return 0;
    }
    if (strcmp(sub, "sim") == 0) {
        if (argc < 3) { ESP_LOGW(TAG, "用法: voice sim <1-%d>", N_CMDS); return 1; }
        int id = atoi(argv[2]);
        if (id < 1 || id > N_CMDS) { ESP_LOGW(TAG, "id 需在 1~%d", N_CMDS); return 1; }
        voice_evt_t ev = { .id = id, .wake = false };
        xQueueOverwrite(s_q, &ev);
        ESP_LOGI(TAG, "已模拟投送 id=%d（走完整安全门；exec=%s）", id, s_exec ? "on 会真的动腿" : "off 只记录");
        return 0;
    }
    if (strcmp(sub, "say") == 0) {
        int id = (argc > 2) ? atoi(argv[2]) : (int)VC_HERE;
        if (id < 0 || id >= (int)VC_COUNT) { ESP_LOGW(TAG, "clip 需在 0~%d", (int)VC_COUNT - 1); return 1; }
        mouth_say((voice_clip_t)id);
        ESP_LOGI(TAG, "已播报片段 %d (%s)", id, clips_name((voice_clip_t)id));
        return 0;
    }
    print_status();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 初始化：按阶段打印片内内存，I1 的验收数据就是这几行。                 */
/* ------------------------------------------------------------------ */
esp_err_t voice_init(void)
{
    mem_snapshot("voice_init 前");

    /* 喇叭先起：它自带 I2S1 TX 自检性质，且词库落在 flash .rodata，不占片内 RAM */
    if (mouth_init() != 0) {
        ESP_LOGW(TAG, "喇叭初始化失败，继续静音运行（耳朵和日志仍可用）");
    }
    char *wn_name = NULL;
    mem_snapshot("喇叭起后");

    if (i2s_mic_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2S 麦克风初始化失败（5/6/7 被占用？）");
        return ESP_FAIL;
    }
    mem_snapshot("麦克风起后");

    /* 分区名必须是 model：esp-sr 按名字找，改名就找不到 */
    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num == 0) {
        ESP_LOGE(TAG, "没找到 model 分区或没烧 srmodels.bin（0x800000）");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "model 分区共 %d 个模型：", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i,
                 s_models->model_name[i] ? s_models->model_name[i] : "?");
    }
    wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    ESP_LOGI(TAG, "唤醒词模型: %s", wn_name ? wn_name : "(无)");
    mem_snapshot("模型加载后");

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "模型分区里没有中文 MultiNet");
        return ESP_ERR_NOT_FOUND;
    }
    s_mn = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_mn->create(mn_name, 8000);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet create 失败（片内内存不足？）");
        return ESP_ERR_NO_MEM;
    }
    mem_snapshot("MultiNet 建立后");

    /* AFE：单麦 "M"、SR 模式、低算力档 */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    s_afe = esp_afe_handle_from_config(afe_cfg);
    s_afe_data = s_afe->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create 失败（片内内存不足？）");
        return ESP_ERR_NO_MEM;
    }
    mem_snapshot("AFE 建立后");

    esp_mn_commands_clear();
    int refused = 0;
    for (int i = 0; i < N_CMDS; i++) {
        if (esp_mn_commands_add(k_cmds[i].id, k_cmds[i].py) != ESP_OK) {
            ESP_LOGW(TAG, "命令词被拒: %s", k_cmds[i].py);
            refused++;
        }
    }
    if (esp_mn_commands_update() != NULL) {
        ESP_LOGE(TAG, "命令词表编译失败（拼音不合法？）");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "已注册 %d 条命令词，拒绝 %d 条", N_CMDS - refused, refused);
    mem_snapshot("命令词注册后");

    s_q = xQueueCreate(1, sizeof(voice_evt_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    s_running = 1;
    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(feed_task,      "v_feed",   FEED_STACK,   NULL,
                                 FEED_PRIO,   &s_h_feed,   VOICE_CORE);
    if (ok != pdPASS) { s_running = 0; return ESP_ERR_NO_MEM; }
    ok = xTaskCreatePinnedToCore(detect_task,    "v_detect", DETECT_STACK, NULL,
                                 DETECT_PRIO, &s_h_detect, VOICE_CORE);
    if (ok != pdPASS) { s_running = 0; return ESP_ERR_NO_MEM; }
    ok = xTaskCreatePinnedToCore(voice_cmd_task, "v_cmd",    CMD_STACK,    NULL,
                                 CMD_PRIO,    &s_h_cmd,    VOICE_CORE);
    if (ok != pdPASS) { s_running = 0; return ESP_ERR_NO_MEM; }

    s_ready = 1;
    vTaskDelay(pdMS_TO_TICKS(800));       /* 让 AFE 跑满几帧再量稳态 */
    mem_snapshot("任务启动后");
    ESP_LOGI(TAG, "语音前端就绪（上电默认 exec=1 放行 / mute=0 播报）。把狗放地上前请先 voice exec off");
    ESP_LOGI(TAG, "调试：voice / voice list / voice sim 1 / voice exec on|off / voice mute on|off");
    return ESP_OK;
}
