/* PC 单测桩：吞掉日志，避免把 IDF 的日志系统拖进桌面编译。 */
#ifndef SPIDER8_STUB_ESP_LOG_H
#define SPIDER8_STUB_ESP_LOG_H

#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

#endif
