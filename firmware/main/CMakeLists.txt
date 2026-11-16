# Spider8 主组件
#
# 用 PRIV_REQUIRES：main 是叶子组件，不对外暴露头文件，私有依赖即可。
# 注意：一旦显式写 REQUIRES/PRIV_REQUIRES，就不会自动继承"公共依赖"，
# 所以 main.c 里用到的每个头文件的组件都得列全 —— 漏一个就是
# "No such file or directory" 或 "not in the requirements list"。
#
#   driver          LEDC PWM（esp_driver_ledc）
#   console         串口命令行（头文件叫 esp_console.h，组件名是 console）
#   nvs_flash       舵机标定值持久化
#   esp_app_format  esp_app_desc.h（固件版本信息）
#   esp_partition   分区表枚举
#   spi_flash       esp_flash_get_size
#   esp_hw_support  esp_chip_info
#   heap            heap_caps_get_total_size
#
# 无线测试台新增：
#   esp_wifi / esp_netif / esp_event   SoftAP 热点
#   esp_http_server                    网页 + /api/* 命令接口
#   lwip                               DNS 应答器用的 BSD socket
#   EMBED_FILES                        把网页嵌进固件，手机离线打开就能用
idf_component_register(
    SRCS "main.c" "servo.c" "gait.c" "motion.c" "console.c" "pca9685.c"
         "cli_out.c" "wireless.c" "ble_ctl.c"
         "voice.c" "mouth.c" "voice_clips.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver console nvs_flash esp_app_format esp_partition spi_flash esp_hw_support heap
                  esp_wifi esp_netif esp_event esp_http_server lwip app_update bt
    EMBED_FILES "web/console.html" "voice_clips.bin"
)
