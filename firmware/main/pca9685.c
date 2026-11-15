/*
 * PCA9685 16 路 PWM 扩展板驱动实现
 *
 * 寄存器（NXP PCA9685 datasheet Rev.4 §7.3，已逐位核对）：
 *   MODE1  0x00   bit7 RESTART  bit5 AI(自动递增)  bit4 SLEEP  bit0 ALLCALL
 *   MODE2  0x01   bit4 INVRT(输出反相!)  bit3 OCH  bit2 OUTDRV(推挽)
 *   LED0_ON_L 0x06，每通道占 4 字节，连着排到 0x45
 *   PRESCALE 0xFE  只在 MODE1.SLEEP=1 时可写；上电默认 0x1E(30) → 200Hz
 *                   value = round(25MHz / (4096 × 目标Hz)) - 1
 *                   50Hz → round(122.07) - 1 = 121（实际 50.03Hz）
 *
 * 【2026-11-08 修复的致命 bug】曾把 0x20 误当 SLEEP、把 MODE2 的 0x10 误当 AI：
 *   - 写 MODE1=0x20 实际只开了 AI，芯片从未进 SLEEP → PRESCALE 写被忽略，
 *     一直跑默认 200Hz（pcadiag 读到 PRESCALE=30）；
 *   - 写 MODE2=0x14 实际置了 INVRT → PWM 输出极性反转；
 *   - 唤醒时又写 MODE1=0x00 把 AI 清掉 → 4 字节连写/连读全塌到同一寄存器
 *     （pcadiag 读到 ON=OFF=0x0101/0x0202），输出锁成恒定电平 → 舵机全不动。
 *   表现：I2C 应答、status、cal 全部正常，唯独信号脚恒 3.3V。
 */
#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "pca9685.h"

static const char *TAG = "pca9685";

#define PCA9685_REG_MODE1       0x00
#define PCA9685_REG_MODE2       0x01
#define PCA9685_REG_LED0_ON_L   0x06
#define PCA9685_REG_PRESCALE    0xFE

/* MODE1 位：RESTART=bit7、AI=bit5、SLEEP=bit4、ALLCALL=bit0。
 * AI 属于 MODE1，不属于 MODE2；MODE2 的 0x10 是 INVRT（输出反相），
 * 置位会让舵机收到极性相反的脉冲，必须保持 0。 */
#define MODE1_RESTART           0x80
#define MODE1_AI                0x20
#define MODE1_SLEEP             0x10
#define MODE1_ALLCALL           0x01
#define MODE2_INVRT             0x10
#define MODE2_OUTDRV            0x04

#define PCA_COUNT_MAX           4095u   /* 12bit 满量程 - 1 */
#define PCA_OSC_HZ              25000000.0f

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;

/* 单次寄存器访问的 I2C 超时（ms）。20ms 节拍下 50ms 已经很宽裕，
 * 真超时会走上层"记录并继续"的降级路径，不会卡死任务。 */
#define I2C_TIMEOUT_MS          50

/* ------------------------------------------------------------------ */
/* 低层寄存器读写                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t reg_write1(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* 读回校验用。连读多字节依赖 MODE1.AI（bit5），不是 MODE2。 */
static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

esp_err_t pca9685_init(void)
{
    const i2c_master_bus_config_t bcfg = {
        .i2c_port             = 0,
        .sda_io_num           = SERVO_PCA_SDA_GPIO,
        .scl_io_num           = SERVO_PCA_SCL_GPIO,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,   /* 短线可用；正式装配建议外接 4.7kΩ */
    };
    esp_err_t err = i2c_new_master_bus(&bcfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SERVO_PCA_ADDR,
        .scl_speed_hz    = SERVO_PCA_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 设备挂载失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 探测：0x40 不应答 = 没接 / 没上电 / 地址跳线改过，直接报错终止启动 */
    uint8_t dummy = 0;
    err = i2c_master_receive(s_dev, &dummy, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X 无应答: %s —— 检查 PCA9685 是否接好（VCC=3.3V、GND 共地）",
                 SERVO_PCA_ADDR, esp_err_to_name(err));
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 1) MODE2：推挽输出、极性不反相。
     *    0x10 是 INVRT，绝不能置位；AI 在 MODE1.bit5，下一步开。 */
    err = reg_write1(PCA9685_REG_MODE2, MODE2_OUTDRV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MODE2 写入失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 2) PRESCALE 只在 SLEEP=1 时可写（datasheet §7.3.4）。
     *    同一次写里把 AI(bit5) 打开，后续 4 字节连写/连读才能自动递增。
     *    注意 SLEEP 是 bit4 = 0x10，不是 0x20（那是 AI）。 */
    err = reg_write1(PCA9685_REG_MODE1, MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "进入 SLEEP 失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_rom_delay_us(500);

    err = reg_write1(PCA9685_REG_PRESCALE, 121);     /* 50Hz */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "50Hz 预分频设置失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_rom_delay_us(500);

    /* 3) 唤醒：清 SLEEP，务必保持 AI=1 */
    err = reg_write1(PCA9685_REG_MODE1, MODE1_AI | MODE1_ALLCALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "唤醒失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_rom_delay_us(500);                           /* datasheet 要求 ≥500µs */

    /* 4) RESTART 让计数器从 0 重启；同样保持 AI=1 */
    err = reg_write1(PCA9685_REG_MODE1, MODE1_AI | MODE1_RESTART | MODE1_ALLCALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RESTART 失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_rom_delay_us(500);

    /* RESTART 位在部分兼容芯片上不自清，再写一次兜底 */
    err = reg_write1(PCA9685_REG_MODE1, MODE1_AI | MODE1_ALLCALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "清 RESTART 失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_rom_delay_us(500);

    /* 5) 逐项读回校验。四项都对才算初始化成功，任何一项不对都直接失败，
     *    绝不允许"写得进去就当成了"——这正是旧版本静默失败的原因。 */
    uint8_t m1 = 0xFF, m2 = 0xFF, ps = 0xFF;
    if ((err = reg_read(PCA9685_REG_MODE1, &m1, 1)) != ESP_OK ||
        (err = reg_read(PCA9685_REG_MODE2, &m2, 1)) != ESP_OK ||
        (err = reg_read(PCA9685_REG_PRESCALE, &ps, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "读回 MODE1/MODE2/PRESCALE 失败: %s", esp_err_to_name(err));
        return err;
    }
    if (m1 & MODE1_SLEEP) {
        ESP_LOGE(TAG, "仍在 SLEEP（MODE1=0x%02X）—— PWM 不会输出", m1);
        return ESP_ERR_INVALID_STATE;
    }
    if (!(m1 & MODE1_AI)) {
        ESP_LOGE(TAG, "MODE1.AI=0（MODE1=0x%02X）—— 4 字节连写会塌到同一寄存器", m1);
        return ESP_ERR_INVALID_STATE;
    }
    if (m2 & MODE2_INVRT) {
        ESP_LOGE(TAG, "MODE2.INVRT=1（MODE2=0x%02X）—— PWM 极性反了", m2);
        return ESP_ERR_INVALID_STATE;
    }
    if (!(m2 & MODE2_OUTDRV)) {
        ESP_LOGE(TAG, "MODE2.OUTDRV=0（MODE2=0x%02X）—— 开漏输出幅度不足", m2);
        return ESP_ERR_INVALID_STATE;
    }
    if (ps != 121) {
        ESP_LOGE(TAG, "PRESCALE=%u（期望 121）—— 频率不是 50Hz，舵机不会响应", ps);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "PCA9685 就绪：地址 0x%02X，SDA=GPIO%d SCL=GPIO%d，50Hz / 12bit（1 计数≈4.88µs）",
             SERVO_PCA_ADDR, SERVO_PCA_SDA_GPIO, SERVO_PCA_SCL_GPIO);
    ESP_LOGI(TAG, "MODE1=0x%02X(AI=1,SLEEP=0) MODE2=0x%02X(INVRT=0,推挽) PRESCALE=%u；OE 必须接 GND",
             m1, m2, ps);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 脉宽写入                                                            */
/* ------------------------------------------------------------------ */

esp_err_t pca9685_write_us(int ch, uint32_t us)
{
    if (ch < 0 || ch > 15 || s_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* µs → 12bit 计数：cnt = us × 4096 / 20000（1 计数 = 4.88µs） */
    float cnt_f = (float)us * 4096.0f / (float)SERVO_PERIOD_US;
    if (cnt_f < 0.0f)         cnt_f = 0.0f;
    if (cnt_f > (float)PCA_COUNT_MAX) cnt_f = (float)PCA_COUNT_MAX;
    const uint32_t cnt = (uint32_t)lroundf(cnt_f);

    /* LEDn_ON 固定 0，正脉冲宽度由 LEDn_OFF 决定。
     * 4 字节连写依赖 MODE2.AI 自动递增。 */
    const uint8_t buf[5] = {
        (uint8_t)(PCA9685_REG_LED0_ON_L + 4 * ch),
        0x00, 0x00,
        (uint8_t)(cnt & 0xFF),
        (uint8_t)(cnt >> 8),
    };

    if (xSemaphoreTake(s_lock, I2C_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    xSemaphoreGive(s_lock);
    return err;
}

/* ------------------------------------------------------------------ */
/* 运行时诊断（串口命令 pcadiag）                                       */
/* ------------------------------------------------------------------ */

esp_err_t pca9685_diag(void)
{
    if (s_dev == NULL) {
        printf("PCA9685 未初始化（当前可能是 LEDC 直连后端）\n");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t m1 = 0xFF, m2 = 0xFF, ps = 0xFF;
    uint8_t led[4] = { 0, 0, 0, 0 };
    esp_err_t err;

    if ((err = reg_read(PCA9685_REG_MODE1, &m1, 1)) != ESP_OK ||
        (err = reg_read(PCA9685_REG_MODE2, &m2, 1)) != ESP_OK ||
        (err = reg_read(PCA9685_REG_PRESCALE, &ps, 1)) != ESP_OK ||
        (err = reg_read(PCA9685_REG_LED0_ON_L, led, 4)) != ESP_OK) {
        printf("PCA9685 寄存器读回失败: %s（查 I2C 接线 / 共地 / VCC=3.3V）\n",
               esp_err_to_name(err));
        return err;
    }

    const uint16_t on  = (uint16_t)led[0] | ((uint16_t)(led[1] & 0x0F) << 8);
    const uint16_t off = (uint16_t)led[2] | ((uint16_t)(led[3] & 0x0F) << 8);
    const float us = (float)off * (float)SERVO_PERIOD_US / 4096.0f;

    printf("\n===== PCA9685 寄存器诊断 =====\n");
    printf("MODE1=0x%02X  AI=%d  SLEEP=%d  RESTART=%d  ALLCALL=%d\n",
           m1, (m1 & MODE1_AI) ? 1 : 0, (m1 & MODE1_SLEEP) ? 1 : 0,
           (m1 & MODE1_RESTART) ? 1 : 0, (m1 & MODE1_ALLCALL) ? 1 : 0);
    printf("MODE2=0x%02X  INVRT=%d  OUTDRV=%d\n",
           m2, (m2 & MODE2_INVRT) ? 1 : 0, (m2 & MODE2_OUTDRV) ? 1 : 0);
    printf("PRESCALE=%u → %.2fHz（期望 121 → 50.03Hz）\n",
           ps, PCA_OSC_HZ / (4096.0f * ((float)ps + 1.0f)));
    printf("CH0: ON=%u OFF=%u → 脉宽 %.0fµs（占空比 %.2f%%，VCC=3.3V 时直流档应读 ≈%.3fV）\n",
           on, off, us, us * 100.0f / (float)SERVO_PERIOD_US,
           3.3f * us / (float)SERVO_PERIOD_US);
    printf("期望：AI=1、SLEEP=0、INVRT=0、PRESCALE=121、ON=0、OFF≈307（1500µs 站姿）\n");
    printf("---- 逐项判读 ----\n");
    printf("%s MODE1.SLEEP=%d → %s\n", (m1 & MODE1_SLEEP) ? "❌" : "✓",
           (m1 & MODE1_SLEEP) ? 1 : 0,
           (m1 & MODE1_SLEEP) ? "计数器没跑，输出锁死" : "芯片已唤醒，计数器在跑");
    printf("%s MODE1.AI=%d → %s\n", (m1 & MODE1_AI) ? "✓" : "❌",
           (m1 & MODE1_AI) ? 1 : 0,
           (m1 & MODE1_AI) ? "连写/连读会正确递增" : "4 字节连写会塌到同一寄存器，ON/OFF 读成相同值");
    printf("%s MODE2.INVRT=%d → %s\n", (m2 & MODE2_INVRT) ? "❌" : "✓",
           (m2 & MODE2_INVRT) ? 1 : 0,
           (m2 & MODE2_INVRT) ? "PWM 极性反了，舵机不认" : "PWM 极性正常（高电平脉冲）");
    printf("%s PRESCALE=%u → %s\n", (ps == 121) ? "✓" : "❌", ps,
           (ps == 121) ? "50Hz" : "不是 50Hz（30=出厂默认 200Hz，说明 SLEEP 没进成、写入被忽略）");
    if (!(m2 & MODE2_OUTDRV)) {
        printf("⚠ MODE2.OUTDRV=0（开漏），输出幅度可能不足\n");
    }
    if (on == off) {
        printf("❌ CH0 ON==OFF：输出会锁成恒定电平，这正是信号脚恒 3.3V / 舵机不动的直接原因\n");
    }
    printf("⚠ OE 是硬件引脚、寄存器读不到：用万用表量 OE 对 GND 应 ≈0V；\n");
    printf("  悬空或接高电平会关掉全部 16 路输出，而芯片照样应答 I2C。\n");
    printf("==============================\n");
    return ESP_OK;
}
