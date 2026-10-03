/*
 * Spider8 - 四足蜘蛛机器人固件
 *
 * 里程碑 M0-1：工程骨架最小可运行版本
 *  目标：打通「编译 → 烧写 → 串口日志」链路，并验证关键硬件资源
 *
 * 功能：
 *  1. 上电打印系统横幅：应用版本、芯片、Flash、PSRAM、内部 RAM、分区表
 *  2. 1Hz 心跳：GPIO4 电平翻转（接 LED+220Ω 到 GND 可见闪烁）+ 串口日志
 *
 * 硬件：ESP32-S3-N16R8（16MB Flash + 8MB Octal PSRAM）
 *
 * 验收标准（对应 docs/02-设计方案.md 里程碑 M0-1）：
 *  - 烧写后串口 115200 可见横幅与心跳日志
 *  - 日志显示 Flash = 16MB、PSRAM = 8192KB（未检测到则打 WARN）
 *  - 分区表输出 ota_0/ota_1/littlefs/coredump 齐全
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

static const char *TAG = "spider8";

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

/* 心跳 GPIO：M0 阶段用 GPIO4（舵机 CH0 同脚位），接 LED 不影响后续开发 */
#define HEARTBEAT_GPIO        GPIO_NUM_4
#define HEARTBEAT_STACK_SIZE  2048   /* 字，不是字节：2048*4 = 8KB 栈 */
#define HEARTBEAT_PRIORITY    2      /* 低优先级，让位给后续的运动任务 */
#define HEARTBEAT_PERIOD_MS   1000

/* 静态分配任务栈与 TCB（项目规范：运行期禁止动态分配） */
static StaticTask_t s_heartbeat_tcb;
static StackType_t  s_heartbeat_stack[HEARTBEAT_STACK_SIZE];

/* ------------------------------------------------------------------ */
/* 系统信息打印                                                        */
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
    ESP_LOGI(TAG, "  Spider8 四足蜘蛛机器人");
    ESP_LOGI(TAG, "  固件   : %s v%s (build %s %s)",
             app->project_name, app->version, app->date, app->time);
    ESP_LOGI(TAG, "  ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "  芯片   : %s rev%d, %d 核",
             chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "UNKNOWN", chip.revision, chip.cores);
    ESP_LOGI(TAG, "  Flash  : %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "  PSRAM  : %lu KB", (unsigned long)psram_kb);
    ESP_LOGI(TAG, "  内部RAM: %lu KB", (unsigned long)internal_kb);
    ESP_LOGI(TAG, "  ----------------------------------------------");
    ESP_LOGI(TAG, "  分区表:");
    log_partitions();
    ESP_LOGI(TAG, "==============================================");

    /* 资源体检：与 N16R8 规格不符时明确告警，避免后续踩坑 */
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
/* 心跳任务                                                            */
/* ------------------------------------------------------------------ */

static void heartbeat_task(void *arg)
{
    bool level = false;
    uint32_t count = 0;

    for (;;) {
        level = !level;
        ESP_ERROR_CHECK(gpio_set_level(HEARTBEAT_GPIO, level));
        ESP_LOGI(TAG, "heartbeat #%lu, free heap=%lu, min-free=%lu",
                 (unsigned long)(++count),
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    print_banner();

    /* 心跳 GPIO 初始化 */
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << HEARTBEAT_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(HEARTBEAT_GPIO, 0));

    /* 静态创建心跳任务：栈使用率可后续用 uxTaskGetStackHighWaterMark 验证 */
    TaskHandle_t handle = xTaskCreateStatic(heartbeat_task, "heartbeat",
                                            HEARTBEAT_STACK_SIZE, NULL,
                                            HEARTBEAT_PRIORITY,
                                            s_heartbeat_stack, &s_heartbeat_tcb);
    if (handle == NULL) {
        ESP_LOGE(TAG, "心跳任务创建失败");
        return;
    }

    ESP_LOGI(TAG, "M0-1 骨架启动完成，进入 1Hz 心跳");
}
