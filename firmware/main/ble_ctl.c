/*
 * Spider8 蓝牙遥控（M3 落地）—— NimBLE GATT server
 *
 * 板子作为 BLE 外设，手机 app 连上后：
 *   - 往 CMD 特征值写 ASCII 命令（如 "walk 50\r\n" / "left"），板子执行；
 *   - 执行结果通过 STATUS 特征值 notify 推回手机。
 * 命令执行复用 cli_exec()，与有线串口、Wi-Fi 网页控制台完全同一套指令。
 *
 * 设计要点：GATT 写回调只把命令塞进队列并唤醒一个独立任务去执行，
 * 回调本身不阻塞，长命令（如脉宽扫描约 19s）不会卡死蓝牙协议栈。
 */
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cli_out.h"
#include "ble_ctl.h"

static const char *TAG = "ble_ctl";

/* 自定义服务 / 特征值 UUID（128-bit）。手机 app 必须严格匹配这三个值。
 *
 * ⚠️ 字节序坑（本版 NimBLE / ESP-IDF 5.5.5 实测）：
 *    BLE_UUID128_INIT 的宏定义是 `.value = { uuid128 }`，【不反转】字节，
 *    而 ATT 传输 128 位 UUID 走【小端】，导致“空中实际 UUID” = 传入字节的反序。
 *    因此下面三行必须是目标字符串
 *      SVC=4b9131c3-c9c5-cc8f-9e45-b51f01c2af4f
 *      CMD=4b9131c3-c9c5-cc8f-9e45-b51f02c2af4f
 *      STS=4b9131c3-c9c5-cc8f-9e45-b51f03c2af4f
 *    的【反序】字节，空中发出的 UUID 才会与手机 App 的 UUID.fromString() 一致。
 *    若改回“正序”字节，板子会广播出反序 UUID（如 4fafc201-…），手机永远找不到服务。 */
#define SPIDER8_SVC_UUID  BLE_UUID128_INIT(0x4f,0xaf,0xc2,0x01,0x1f,0xb5,0x45,0x9e,0x8f,0xcc,0xc5,0xc9,0xc3,0x31,0x91,0x4b)
#define SPIDER8_CMD_UUID  BLE_UUID128_INIT(0x4f,0xaf,0xc2,0x02,0x1f,0xb5,0x45,0x9e,0x8f,0xcc,0xc5,0xc9,0xc3,0x31,0x91,0x4b)
#define SPIDER8_STS_UUID  BLE_UUID128_INIT(0x4f,0xaf,0xc2,0x03,0x1f,0xb5,0x45,0x9e,0x8f,0xcc,0xc5,0xc9,0xc3,0x31,0x91,0x4b)

static const ble_uuid128_t gatt_svc_uuid = SPIDER8_SVC_UUID;
static const ble_uuid128_t gatt_cmd_uuid = SPIDER8_CMD_UUID;
static const ble_uuid128_t gatt_sts_uuid = SPIDER8_STS_UUID;

static uint16_t s_cmd_handle;   /* CMD 特征值句柄（手机写） */
static uint16_t s_sts_handle;   /* STATUS 特征值句柄（手机读 / 收 notify） */

/* 命令执行桥接：写回调只把命令塞进队列，独立任务负责执行，避免阻塞 BLE 栈 */
static char          s_cmd[CLI_CMD_MAX];
static uint16_t      s_cmd_conn;

/* 当前是否有手机 BLE 连接。wireless.c 的 httpd 看门狗会查它：
   BLE 遥控期间绝不能因网页自检失败就 esp_restart()，否则手机必然
   status=8 断连、重启后重新广播又自动连上（2026-11-14 实测坑）。 */
static volatile bool s_connected = false;

bool ble_ctl_is_connected(void)
{
    return s_connected;
}
static char          s_out[CLI_OUT_MAX];
static SemaphoreHandle_t s_cmd_sem;

static uint8_t       own_addr_type = BLE_OWN_ADDR_PUBLIC;

/* ------------------------------------------------------------------ */
/* GATT 服务定义                                                       */
/* ------------------------------------------------------------------ */
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_cmd_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_cmd_handle,
            },
            {
                .uuid = &gatt_sts_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_sts_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ------------------------------------------------------------------ */
/* GATT 访问回调                                                       */
/* ------------------------------------------------------------------ */
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle != s_cmd_handle) return BLE_ATT_ERR_UNLIKELY;
        int n = ctxt->om->om_len;
        if (n <= 0) return 0;                       /* 空写忽略 */
        if (n >= CLI_CMD_MAX) n = CLI_CMD_MAX - 1;  /* 超长截断，防溢出 */
        memcpy(s_cmd, ctxt->om->om_data, n);
        s_cmd[n] = '\0';
        s_cmd_conn = conn_handle;
        xSemaphoreGive(s_cmd_sem);                  /* 唤醒执行任务；回调不阻塞 */
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle != s_sts_handle) return BLE_ATT_ERR_UNLIKELY;
        int n = strlen(s_out);
        if (n > ctxt->om->om_len) n = ctxt->om->om_len;  /* 超长读则截断 */
        memcpy(ctxt->om->om_data, s_out, n);
        ctxt->om->om_len = n;
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* 把执行结果按【本次连接实际协商出的 ATT MTU】分包 notify。
 *
 * ⚠️ 血泪坑（2026-11-14，手机「一发指令、几秒后断 status=8」真因）：
 *   绝不能用 ble_att_preferred_mtu() —— 那是本地【期望】值（本项目设 256），
 *   不是链路实际值。安卓默认 ATT_MTU=23（每片净荷仅 20 字节）且不主动协商，
 *   按 253 字节发 notify 会发出超长 PDU 并撑爆 msys mbuf 池（本项目仅 36 块），
 *   手机端链路层异常 → 监督超时断开。
 *   只有发指令才产生 notify，所以症状是「不发就稳、一发就断」；而电脑蓝牙
 *   会自动协商大 MTU，本机怎么测都正常（这个坑差点被本机测试放过去）。
 */
static void ble_send_status(uint16_t conn)
{
    uint16_t att_mtu = ble_att_mtu(conn);   /* 实际协商值；连接无效/已断返回 0 */
    if (att_mtu < 23) {
        ESP_LOGW(TAG, "BLE 回话跳过：连接无效 att_mtu=%u", (unsigned)att_mtu);
        return;
    }
    int mtu = (int)att_mtu - 3;             /* notify 的 ATT 头占 3 字节 */
    if (mtu < 1) mtu = 20;

    int total = (int)strlen(s_out);
    /* 严格【单包】回话：蓝牙回话只用于「命令已执行 / 错误提示」，
       把结果截到本次连接的一片净荷内，任何 MTU（23~517）下都只发 1 条 notify。
       这样彻底消除「多片 notify 风暴 / 片间延时」这一整类变量——
       手机报 MTU=256 仍 status=8 后进一步收敛。
       要看完整 status 用串口或网页 /api/status，蓝牙只回一句要点。 */
    if (total > mtu) total = mtu;

    int off = 0;
    if (total > 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_out, total);
        if (!om) {
            ESP_LOGW(TAG, "notify mbuf 不足，回话未发");
        } else {
            /* 注意：notify_custom 内部在 done 分支会 os_mbuf_free_chain(txom)，
               成功与失败都已接管释放，这里【不要】再手动释放，否则 double free。 */
            int rc = ble_gatts_notify_custom(conn, s_sts_handle, om);
            if (rc != 0) {
                ESP_LOGW(TAG, "notify 失败 rc=%d", rc);
            } else {
                off = total;
            }
        }
    }
    ESP_LOGI(TAG, "BLE 回话 att_mtu=%u 净荷=%dB 实发=%dB/总%d 单包",
             (unsigned)att_mtu, mtu, off, (int)strlen(s_out));
}

/* 命令执行任务：与 HTTP / REPL 共用 cli_exec() 内部的执行锁，天然串行 */
static void ble_cmd_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_cmd_sem, portMAX_DELAY) == pdTRUE) {
            int rc = cli_exec(s_cmd, s_out, sizeof(s_out));
            (void)rc;
            if (s_cmd_conn) {
                ble_send_status(s_cmd_conn);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 连接参数安全下限                                                    */
/* ------------------------------------------------------------------ */
/* 2026-11-14 三轮（「基本不断了，但时间长了还是会断」）真因：
 * 手机安卓在 GATT 操作期间会自己请求 HIGH 参数，板子日志实测
 *   BLE 手机请求连接参数 → 最终生效 itvl=6 lat=0 to=500
 * 即 7.5ms 间隔 + 仅 5 秒监督超时。Wi-Fi/BLE 共用 2.4GHz，路由器一忙
 * 连丢几个连接事件就被手机判超时 status=8；短时间看不出来，放久了必现。
 * 以前 CONN_UPDATE_REQ 只打日志、直接接受，等于把 5s 超时放进来。
 * 现在把「本机期望参数」钉在 30~50ms / 20s，长按也不会因为瞬时干扰掉线。 */
#define SP8_ITVL_MIN_SAFE   24      /* 30ms */
#define SP8_ITVL_MAX_SAFE   40      /* 50ms */
#define SP8_TIMEOUT_SAFE    2000    /* 20s  */

static void clamp_conn_params(struct ble_gap_upd_params *p)
{
    if (!p) return;
    if (p->itvl_min < SP8_ITVL_MIN_SAFE) p->itvl_min = SP8_ITVL_MIN_SAFE;
    if (p->itvl_max < p->itvl_min)       p->itvl_max = p->itvl_min;
    if (p->itvl_max < SP8_ITVL_MAX_SAFE) p->itvl_max = SP8_ITVL_MAX_SAFE;
    if (p->latency > 4)                  p->latency = 4;
    if (p->supervision_timeout < SP8_TIMEOUT_SAFE) {
        p->supervision_timeout = SP8_TIMEOUT_SAFE;
    }
}

/* 兜底：手机若仍强行把参数压到危险区，参数更新完成后再申请一次安全参数。
 * 每条连接最多 3 次，避免和手机来回拉锯。 */
static int s_param_fixups = 0;

/* ------------------------------------------------------------------ */
/* GAP 事件与广播                                                      */
/* ------------------------------------------------------------------ */
static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)"Spider8";
    fields.name_len = strlen("Spider8");

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播字段失败: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    /* 供 wireless.c 的 httpd 看门狗查询：BLE 遥控期间禁止自动重启 */
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
        s_connected = true;
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        s_connected = false;
    }

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_param_fixups = 0;
            struct ble_gap_conn_desc d;
            int frc = ble_gap_conn_find(event->connect.conn_handle, &d);
            if (frc == 0) {
                ESP_LOGI(TAG, "BLE 已连接 conn=%d itvl=%u lat=%u to=%u role=%d",
                         event->connect.conn_handle, d.conn_itvl, d.conn_latency,
                         d.supervision_timeout, d.role);
            } else {
                ESP_LOGI(TAG, "BLE 已连接 conn=%d", event->connect.conn_handle);
            }
            /* 对症「手机 status=8（监督超时）」：
               短间隔(15ms)下 ESP32 与 Wi-Fi 抢 2.4GHz 射频，连续丢几个
               连接事件就会被手机判超时。改为宽松间隔 + 长监督超时：
                 itvl 单位 1.25ms → 24~40 = 30~50ms
                 supervision_timeout 单位 10ms → 2000 = 20s
               2026-11-14 二轮：10s 在长时间空闲 + 手机息屏 / 路由器繁忙时
               仍会被判超时，放宽到 20s；latency=2 让板子在 Wi-Fi 发射的
               几个事件里可以合法跳过，不必硬凑每个连接事件。
               ⚠️ 手机侧 App 已不再调 requestConnectionPriority，避免两边
                  同时发起参数更新互相打架。 */
            struct ble_gap_upd_params up;
            memset(&up, 0, sizeof(up));
            up.itvl_min = SP8_ITVL_MIN_SAFE;   /* 30ms */
            up.itvl_max = SP8_ITVL_MAX_SAFE;   /* 50ms */
            up.latency  = 2;
            up.supervision_timeout = SP8_TIMEOUT_SAFE;  /* 20s */
            int urc = ble_gap_update_params(event->connect.conn_handle, &up);
            if (urc != 0) {
                ESP_LOGW(TAG, "请求连接参数更新失败: %d", urc);
            }
        } else {
            ESP_LOGW(TAG, "BLE 连接失败 status=%d，重新开始广播", event->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE 参数更新结果 status=%d", event->conn_update.status);
        {
            struct ble_gap_conn_desc d2;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &d2) == 0) {
                ESP_LOGI(TAG, "  最终生效 itvl=%u lat=%u to=%u",
                         d2.conn_itvl, d2.conn_latency, d2.supervision_timeout);
                /* 手机侧若绕过我们的期望参数强行压到 7.5ms / 5s，再拉回来 */
                if (d2.conn_itvl < 16 || d2.supervision_timeout < 1000) {
                    if (s_param_fixups < 3) {
                        s_param_fixups++;
                        struct ble_gap_upd_params fix;
                        memset(&fix, 0, sizeof(fix));
                        fix.itvl_min = SP8_ITVL_MIN_SAFE;
                        fix.itvl_max = SP8_ITVL_MAX_SAFE;
                        fix.latency  = 2;
                        fix.supervision_timeout = SP8_TIMEOUT_SAFE;
                        ESP_LOGW(TAG, "  检测到危险参数，重新申请安全参数（第 %d 次）",
                                 s_param_fixups);
                        (void)ble_gap_update_params(event->conn_update.conn_handle, &fix);
                    }
                }
            }
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
    {
        const struct ble_gap_upd_params *peer = event->conn_update_req.peer_params;
        const char *kind = (event->type == BLE_GAP_EVENT_CONN_UPDATE_REQ) ? "LL" : "L2CAP";
        if (peer) {
            ESP_LOGI(TAG, "BLE 手机请求参数(%s) itvl=%u~%u lat=%u to=%u",
                     kind, peer->itvl_min, peer->itvl_max,
                     peer->latency, peer->supervision_timeout);
        }
        /* CONN_UPDATE_REQ：self_params 是 host 给的副本，回填后由
           ble_gap_tx_param_pos_reply() 发给控制器。
           L2CAP_UPDATE_REQ：本版 NimBLE 没填 self_params，host 直接拿
           peer_params 去 ble_gap_update_params()；那个实参是栈上可写结构，
           就地夹紧即可（等价于「接受，但用安全值」），比硬拒绝更不容易
           触发手机反复重试。 */
        struct ble_gap_upd_params *self = event->conn_update_req.self_params;
        if (self) {
            clamp_conn_params(self);
            ESP_LOGI(TAG, "  回填安全参数 itvl=%u~%u lat=%u to=%u",
                     self->itvl_min, self->itvl_max,
                     self->latency, self->supervision_timeout);
        } else if (peer) {
            clamp_conn_params((struct ble_gap_upd_params *)peer);
            ESP_LOGI(TAG, "  就地夹紧 itvl=%u~%u lat=%u to=%u",
                     peer->itvl_min, peer->itvl_max,
                     peer->latency, peer->supervision_timeout);
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU 协商完成 conn=%d cid=%u mtu=%u",
                 event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 断开 conn=%d reason=%d，重新开始广播",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        s_cmd_conn = 0;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* NimBLE 生命周期                                                     */
/* ------------------------------------------------------------------ */
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset，原因=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr 失败: %d", rc);
        return;
    }
    rc = ble_svc_gap_device_name_set("Spider8");
    if (rc != 0) {
        ESP_LOGE(TAG, "设置设备名失败: %d", rc);
    }
    advertise();
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task 启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_ctl_init(void)
{
    int rc;

    s_cmd_sem = xSemaphoreCreateBinary();
    if (!s_cmd_sem) {
        ESP_LOGE(TAG, "命令信号量创建失败");
        return ESP_ERR_NO_MEM;
    }
    s_out[0] = '\0';
    s_cmd_conn = 0;

    /* NimBLE 在 ESP-IDF 里，nimble_port_init() 内部已经会初始化并启用蓝牙
       控制器（esp_nimble_hci_and_controller_init）。不要在外面再手动调
       esp_bt_controller_init/enable，否则会重复初始化返回 ESP_ERR_INVALID_STATE。 */
    rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.sm_io_cap       = BLE_SM_IO_CAP_NO_IO;  /* 测试期不做配对鉴权 */

    ble_att_set_preferred_mtu(256);

    /* NimBLE v5.5 起以下初始化函数返回 void（旧版返回 int） */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "ble_gatts_add_svcs 失败: %d", rc); return ESP_FAIL; }

    nimble_port_freertos_init(ble_host_task);

    if (xTaskCreate(ble_cmd_task, "ble_cmd", 6144, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "命令执行任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BLE 初始化完成，设备名 Spider8，等待手机连接");
    return ESP_OK;
}
