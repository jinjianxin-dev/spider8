/*
 * Spider8 - 四足机器狗固件
 *
 * 里程碑 M0-2：舵机驱动 + 校准框架 + 步态引擎
 *
 * 本版本实现（对应 docs/05-M0里程碑.md 的 M0-2 验收项）：
 *   1. 8 路 LEDC 50Hz PWM，14bit，每关节独立 trim/dir/软限位
 *   2. 两连杆逆运动学步态引擎（波步/小跑/溜蹄/跳跃）+ 差速转向
 *   3. 动作状态机：站立 / 蹲下 / 趴下 / 握手 / 挥手
 *   4. 串口 CLI：pose / walk / trot / cal / trim / status / demo
 *
 * 硬件：ESP32-S3-N16R8（16MB Flash + 8MB Octal PSRAM），8×MG90S
 *
 * ── 上电前的装配要求 ──────────────────────────────────────────────
 * 先把四条腿摆成"站立姿态"（髋轴离地 104.0mm，膝屈 -38°），
 * 然后把舵机臂装到 90°。这样驱动里的 90° 就等于标准站姿。
 * 装不精确没关系，用 `cal` 点动 + `trim` 微调 + `save` 存 NVS。
 * ────────────────────────────────────────────────────────────────
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"

#include "cli_out.h"
#include "config.h"
#include "console.h"
#include "gait.h"
#include "motion.h"
#include "servo.h"
#include "wireless.h"
#include "ble_ctl.h"
#include "voice.h"

static const char *TAG = "spider8";

/* ------------------------------------------------------------------ */
/* 系统信息                                                            */
/* ------------------------------------------------------------------ */

static void log_partitions(void)
{
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    int n = 0;
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGI(TAG, "  分区[%d] %-10s type=%d subtype=%d addr=0x%06lx size=%luKB",
                 n++, p->label, p->type, p->subtype,
                 (unsigned long)p->address, (unsigned long)(p->size / 1024));
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
}

static void print_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    esp_chip_info_t chip = { 0 };
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    const uint32_t psram_kb    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;
    const uint32_t internal_kb = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  Spider8 四足机器狗");
    ESP_LOGI(TAG, "  固件   : %s v%s (build %s %s)",
             app->project_name, app->version, app->date, app->time);
    ESP_LOGI(TAG, "  ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "  芯片   : %s rev%d, %d 核",
             chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "UNKNOWN", chip.revision, chip.cores);
    ESP_LOGI(TAG, "  Flash  : %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "  PSRAM  : %lu KB", (unsigned long)psram_kb);
    ESP_LOGI(TAG, "  内部RAM: %lu KB", (unsigned long)internal_kb);
    /* 诊断「手机一发指令就断」时最有用的一行：区分是掉压/崩溃重启，
       还是用户手动上电。ESP_RST_BROWNOUT=舵机启动把 5V 拉塌了。 */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        ESP_LOGI(TAG, "  复位原因: %d%s%s", (int)rr,
                 rr == ESP_RST_BROWNOUT ? "  ← 掉压！舵机启动把 5V 拉塌了，换带载更强的电源" : "",
                 rr == ESP_RST_PANIC    ? "  ← 软件崩溃（panic），请抓串口回溯" : "");
    }
    ESP_LOGI(TAG, "  ----------------------------------------------");
    ESP_LOGI(TAG, "  分区表:");
    log_partitions();
    ESP_LOGI(TAG, "==============================================");

    if (flash_size / (1024 * 1024) < 16) {
        ESP_LOGW(TAG, "Flash 只有 %luMB，预期 16MB（N16R8）——请检查开发板型号",
                 (unsigned long)(flash_size / (1024 * 1024)));
    }
    if (psram_kb < 8192) {
        ESP_LOGW(TAG, "PSRAM 只有 %luKB，预期 8192KB —— N16R8 应为 Octal 8MB，"
                      "请确认 sdkconfig 已启用 CONFIG_SPIRAM_MODE_OCT", (unsigned long)psram_kb);
    }
}

/* ------------------------------------------------------------------ */
/* 运动学自检                                                          */
/* ------------------------------------------------------------------ */

static void kinematics_selftest(void)
{
    static const struct {
        const char *name;
        float knee;
    } poses[] = {
        { "装配基准", GAIT_STAND_KNEE_DEG },
        { "站立",     GAIT_DEFAULT_STAND_KNEE_DEG },
        { "蹲下",     GAIT_DEFAULT_STAND_KNEE_DEG + POSE_CROUCH_DK },
        { "趴下",     GAIT_DEFAULT_STAND_KNEE_DEG + POSE_LIE_DK },
    };

    ESP_LOGI(TAG, "----------------------------------------------");
    ESP_LOGI(TAG, "  运动学自检（大腿 %.0fmm + 小腿 %.0fmm）",
             GAIT_THIGH_MM, GAIT_SHIN_MM);

    for (size_t i = 0; i < sizeof(poses) / sizeof(poses[0]); i++) {
        const float knee  = poses[i].knee;
        const float hip   = gait_hip_for_knee(knee);
        const float h     = gait_height_for_knee(knee);
        const float s_hip = servo_joint_to_servo(0, hip);
        const float s_kn  = servo_joint_to_servo(1, knee);

        ESP_LOGI(TAG, "  %s: 髋%6.1f° 膝%7.1f° → 髋轴离地 %5.1fmm | 舵机 %5.1f° / %5.1f°",
                 poses[i].name, hip, knee, h, s_hip, s_kn);

        const bool ok = (s_hip > 0.0f && s_hip < 180.0f &&
                         s_kn  > 0.0f && s_kn  < 180.0f);
        if (!ok) {
            ESP_LOGE(TAG, "    ✗ 舵机角超出行程，检查机械尺寸/软限位设置");
        }
    }
    ESP_LOGI(TAG, "----------------------------------------------");
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* 必须最早调用：接管 ESP_LOG → 环形日志，无线网页才能看到启动日志；
     * 同时建立有线 REPL / 无线 HTTP 共用的命令执行锁。 */
    ESP_ERROR_CHECK(cli_out_init());

    print_banner();

    /* NVS：标定值存储。首次启动或分区格式变更时擦除重建。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化（%s），擦除中", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 舵机：直接摆到站立姿态上电，避免先回中位再跳过去 */
    float stand[J_COUNT];
    motion_stand_angles(stand);
    ESP_ERROR_CHECK(servo_init(stand));

    kinematics_selftest();

    ESP_ERROR_CHECK(motion_init());
    ESP_ERROR_CHECK(motion_start());

    if (console_init() != ESP_OK) {
        ESP_LOGW(TAG, "串口命令行不可用，固件仍会保持站立姿态");
    }

    /* 无线测试台是「加分项」：起不来就退回有线串口，绝不影响运动功能 */
    if (wireless_init() != ESP_OK) {
        ESP_LOGW(TAG, "无线测试台启动失败，请插 USB 线用串口命令行继续测试");
    }

    /* 蓝牙遥控（M3）：NimBLE GATT server，手机 app 通过 BLE 写命令。
       已修复 IDF 5.5 崩溃根因（漏掉 esp_bt_controller_init/enable），
       现在与 Wi-Fi 网页控制台、有线串口共用同一套 cli_exec 命令。
       起不来也只是日志告警，不影响运动 / 网页功能。 */
    if (ble_ctl_init() != ESP_OK) {
        ESP_LOGW(TAG, "蓝牙遥控启动失败，请检查 sdkconfig 是否启用 NimBLE；运动与网页控制台不受影响");
    }

#ifdef CONFIG_SPIDER8_VOICE_ENABLED
    /* 语音前端（I1）：放在所有既有功能之后启动，起不来也只是没耳朵，
     * 不影响站立、串口、蓝牙。语音任务全绑 core0，伺服在 core1。 */
    if (voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音前端启动失败：检查 model 分区(0x800000)是否已烧 srmodels.bin、片内内存是否够");
    }
#else
    ESP_LOGI(TAG, "语音前端已用 CONFIG_SPIDER8_VOICE_ENABLED=n 编译关闭");
#endif

    /* OTA 升级回滚保护：开启 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 后，
       新槽首次启动处于「待确认」状态，必须由本函数确认，否则下次重启会
       自动回退到上一版可正常启动的固件（刷坏也能救回来）。 */
    esp_err_t ota_valid = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_valid != ESP_OK) {
        ESP_LOGD(TAG, "OTA 本槽非待确认状态（正常）: %s", esp_err_to_name(ota_valid));
    }

    ESP_LOGI(TAG, "M0-2 启动完成。试试: status / demo / walk / pose crouch");
}
