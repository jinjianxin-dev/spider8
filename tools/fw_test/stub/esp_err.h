/* PC 单测桩：只提供 motion.c 需要的 ESP-IDF 错误码类型。
 * 真实固件编译时用的是 ESP-IDF 的 esp_err.h，这里不会被引用。 */
#ifndef SPIDER8_STUB_ESP_ERR_H
#define SPIDER8_STUB_ESP_ERR_H

typedef int esp_err_t;
#define ESP_OK          0
#define ESP_ERR_NO_MEM  0x101

#endif
