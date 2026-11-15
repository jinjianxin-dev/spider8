/*
 * Spider8 舵机驱动实现
 *
 * 输出后端二选一（menuconfig → "Spider8 舵机后端"，Kconfig.projbuild）：
 *   CONFIG_SERVO_BACKEND_LEDC     —— 8 个 GPIO 直连（14bit，1 份≈1.22µs）
 *   CONFIG_SERVO_BACKEND_PCA9685  —— I2C 扩展板（12bit，1 计数≈4.88µs）
 * 两个后端对上层完全同构：backend_init() / backend_write_us(j, µs)。
 *
 * 三道保护，从外到内：
 *   1. 软限位    —— 关节角超范围直接夹住（机构别顶死）
 *   2. 电子硬限位 —— 舵机角夹在 0~180°，防止标定手滑输出 3000µs
 *   3. 单拍限速   —— 20ms 内最多走 12°（600°/s），防止指令突跳把齿轮打崩
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_SERVO_BACKEND_PCA9685
#include "pca9685.h"
#else
#include "driver/ledc.h"
#endif

#include "esp_log.h"
#include "nvs.h"

#include "gait.h"
#include "servo.h"

static const char *TAG = "servo";

#define NVS_NS      "servo8"
#define NVS_KEY_CAL "cal"

#if !CONFIG_SERVO_BACKEND_PCA9685
/* LEDC 后端：通道 0~7 ↔ 引脚，顺序与 J_xxx 宏一致 */
static const int k_pin[J_COUNT] = {
    SERVO_PIN_FL_HIP,  SERVO_PIN_FL_KNEE,
    SERVO_PIN_FR_HIP,  SERVO_PIN_FR_KNEE,
    SERVO_PIN_RL_HIP,  SERVO_PIN_RL_KNEE,
    SERVO_PIN_RR_HIP,  SERVO_PIN_RR_KNEE,
};
#endif

/* 每关节标定。dir 默认 +1，装配后若某条腿方向相反改成 -1。 */
typedef struct {
    int8_t  dir;
    float   trim_deg;
    int16_t us_mid;
    int16_t us_span;
    float   lim_min;    /* 关节角软限位 */
    float   lim_max;
} servo_cal_t;

static servo_cal_t s_cal[J_COUNT];

/* 关节角 → 舵机90° 的基准：站立姿态（髋 19°、膝 -38°）。
 * 装配时把腿摆成站姿、舵机臂装到 90°，之后一切从这算起。 */
static float s_ref_joint[J_COUNT];

/* 最近一次实际下发值 */
static float s_joint[J_COUNT];
static float s_servo[J_COUNT];
static uint32_t s_clamp_n;

/* raw 模式：只允许串口直接写脉宽，冻结 servo_apply */
static bool s_raw = false;

/* ------------------------------------------------------------------ */
/* 映射                                                                */
/* ------------------------------------------------------------------ */

float servo_joint_to_servo(int j, float joint_deg)
{
    return 90.0f + (joint_deg - s_ref_joint[j]) * (float)s_cal[j].dir
                  + s_cal[j].trim_deg;
}

/* 舵机角 → 目标脉宽（µs），300~2700 兜底夹紧 */
static float servo_deg_to_us(int j, float servo_deg)
{
    const float us = (float)s_cal[j].us_mid +
                     (servo_deg - 90.0f) * (float)s_cal[j].us_span / 180.0f;

    float us_c = us;
    if (us_c < (float)SERVO_US_RAW_MIN) us_c = (float)SERVO_US_RAW_MIN;  /* 极端兜底 */
    if (us_c > (float)SERVO_US_RAW_MAX) us_c = (float)SERVO_US_RAW_MAX;
    return us_c;
}

/* ------------------------------------------------------------------ */
/* 输出后端（编译期二选一）                                            */
/* ------------------------------------------------------------------ */

#if CONFIG_SERVO_BACKEND_PCA9685

static esp_err_t backend_init(void)
{
    return pca9685_init();
}

/* PCA9685：us 直接写 OFF 计数（12bit，1 计数≈4.88µs） */
static esp_err_t backend_write_us(int j, int us)
{
    return pca9685_write_us(j, (uint32_t)us);
}

void servo_out_label(int j, char *buf, int len)
{
    snprintf(buf, len, "PCA%-3d", j);       /* PCA9685 的 0~7 号排针 */
}

#else /* CONFIG_SERVO_BACKEND_LEDC */

static esp_err_t backend_init(void)
{
    /* LEDC 定时器：50Hz，14bit */
    const ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_PWM_RES,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = SERVO_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC 定时器配置失败: %s", esp_err_to_name(err));
        return err;
    }

    for (int j = 0; j < J_COUNT; j++) {
        const ledc_channel_config_t ccfg = {
            .gpio_num   = k_pin[j],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)j,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0,
        };
        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CH%d(GPIO%d) 配置失败: %s", j, k_pin[j], esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* LEDC：us → 14bit 计数 → set_duty + update_duty */
static esp_err_t backend_write_us(int j, int us)
{
    uint32_t cnt = (uint32_t)((float)us * (float)SERVO_PWM_FULL / (float)SERVO_PERIOD_US);
    if (cnt > SERVO_PWM_FULL - 1) {
        cnt = SERVO_PWM_FULL - 1;
    }

    /* 这里不用 ESP_ERROR_CHECK：50Hz 热路径上若真失败，
     * 直接 abort 会变成"重启-失败-重启"死循环，反而看不到日志。
     * 改为记录并继续，其他通道仍然工作。 */
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)j, cnt);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)j);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH%d 写占空比失败: %s", j, esp_err_to_name(err));
    }
    return err;
}

void servo_out_label(int j, char *buf, int len)
{
    snprintf(buf, len, "GPIO%-2d", k_pin[j]);
}

#endif /* 后端选择 */

/* 统一入口：写脉宽，成功才更新读回值（失败时保持上一拍，不自欺） */
static esp_err_t servo_write_us(int j, int us)
{
    const esp_err_t err = backend_write_us(j, us);
    if (err == ESP_OK) {
        s_servo[j] = 90.0f + ((float)us - (float)s_cal[j].us_mid) * 180.0f
                            / (float)s_cal[j].us_span;
    }
    return err;
}

static void servo_write_ch(int j, float servo_deg)
{
    servo_write_us(j, (int)servo_deg_to_us(j, servo_deg));
}

/* ------------------------------------------------------------------ */
/* 电气验证：原始脉宽                                                  */
/* ------------------------------------------------------------------ */

void servo_raw_enter(void)
{
    s_raw = true;
}

void servo_raw_exit(void)
{
    s_raw = false;
}

bool servo_raw_active(void)
{
    return s_raw;
}

int servo_pin(int j)
{
#if CONFIG_SERVO_BACKEND_PCA9685
    return (j >= 0 && j < J_COUNT) ? j : -1;    /* PCA9685 通道号 */
#else
    return (j >= 0 && j < J_COUNT) ? k_pin[j] : -1;
#endif
}

uint32_t servo_raw_us(int j, int us)
{
    if (j < 0 || j >= J_COUNT) {
        return 0;
    }

    /* 电气护栏：即使上层手滑传入 20000，也只输出到 SERVO_US_RAW_MAX。
     * 这里不走 trim/dir/软限位 —— 目的是测裸脉宽真实端点；
     * 360° 大行程位置舵机的满程端点可能超过 2500µs，上限放宽到 3000µs。 */
    if (us < SERVO_US_RAW_MIN) us = SERVO_US_RAW_MIN;
    if (us > SERVO_US_RAW_MAX) us = SERVO_US_RAW_MAX;

    esp_err_t err = servo_write_us(j, us);   /* 写失败时 s_servo 不更新 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH%d 原始脉宽写入失败: %s", j, esp_err_to_name(err));
        return 0;
    }

    /* servo_write_us 已把脉宽反算成等效舵机角存进 s_servo：
     * 退出 raw 后，单拍限速从"当前实际位置"接续，姿态回位不会突跳。 */
    return (uint32_t)us;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

esp_err_t servo_init(const float joint_deg[J_COUNT])
{
    /* 基准角：站立姿态 */
    s_ref_joint[0] = s_ref_joint[2] = s_ref_joint[4] = s_ref_joint[6] =
        gait_hip_for_knee(GAIT_STAND_KNEE_DEG);
    s_ref_joint[1] = s_ref_joint[3] = s_ref_joint[5] = s_ref_joint[7] =
        GAIT_STAND_KNEE_DEG;

    for (int j = 0; j < J_COUNT; j++) {
        s_cal[j].dir      = 1;
        s_cal[j].trim_deg = 0.0f;
        s_cal[j].us_mid   = SERVO_US_MID_DEFAULT;
        s_cal[j].us_span  = SERVO_US_SPAN_DEFAULT;

        if (JOINT_IS_HIP(j)) {
            s_cal[j].lim_min = -45.0f;
            s_cal[j].lim_max =  75.0f;
        } else {
            s_cal[j].lim_min = -100.0f;
            s_cal[j].lim_max =    8.0f;
        }
        s_joint[j] = joint_deg ? joint_deg[j] : 0.0f;
        s_servo[j] = 90.0f;
    }
    s_clamp_n = 0;

    /* 先读 NVS 标定值（失败不致命，用默认值继续） */
    esp_err_t err = servo_load_cal();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "未读到 NVS 标定值（%s），使用默认值", esp_err_to_name(err));
    }

    /* 输出后端：LEDC 定时器/通道 或 PCA9685 I2C 初始化 */
    err = backend_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "输出后端初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 直接摆到初始姿态：上电瞬间就位，避免先回 90° 再跳过去 */
    const float ref_hip = gait_hip_for_knee(GAIT_STAND_KNEE_DEG);
    ESP_LOGI(TAG, "基准：髋 %.1f°/膝 %.1f° = 舵机 90°，髋轴离地 %.1fmm",
             ref_hip, GAIT_STAND_KNEE_DEG, gait_height_for_knee(GAIT_STAND_KNEE_DEG));

    for (int j = 0; j < J_COUNT; j++) {
        const float sv = servo_joint_to_servo(j, s_joint[j]);
        servo_write_ch(j, sv);
    }

    /* 打印映射表，方便对照实物（GPIO 直连或 PCA9685 排针） */
    for (int j = 0; j < J_COUNT; j++) {
        const float sv = servo_joint_to_servo(j, s_joint[j]);
        const uint32_t us = (uint32_t)((float)s_cal[j].us_mid +
                            (sv - 90.0f) * (float)s_cal[j].us_span / 180.0f);
        char lbl[16];
        servo_out_label(j, lbl, sizeof(lbl));
        ESP_LOGI(TAG, "CH%d %-5s %s %s 关节%6.1f° → 舵机%6.1f° → %4luus",
                 j, lbl, JOINT_IS_HIP(j) ? "髋" : "膝",
                 (j == J_FL_HIP || j == J_FL_KNEE) ? "FL" :
                 (j == J_FR_HIP || j == J_FR_KNEE) ? "FR" :
                 (j == J_RL_HIP || j == J_RL_KNEE) ? "RL" : "RR",
                 s_joint[j], sv, (unsigned long)us);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 输出                                                                */
/* ------------------------------------------------------------------ */

void servo_apply(const float joint_deg[J_COUNT], bool cal_mode)
{
    if (joint_deg == NULL) {
        return;
    }

    /* raw 电气测试期间任何姿态写入都不生效，否则呼吸灯会被 20ms 任务踩掉 */
    if (s_raw) {
        return;
    }

    for (int j = 0; j < J_COUNT; j++) {
        float a = joint_deg[j];

        /* 1) 软限位 */
        if (!cal_mode) {
            if (a < s_cal[j].lim_min) {
                a = s_cal[j].lim_min;
                s_clamp_n++;
            } else if (a > s_cal[j].lim_max) {
                a = s_cal[j].lim_max;
                s_clamp_n++;
            }
        }
        s_joint[j] = a;

        float sv = servo_joint_to_servo(j, a);

        /* 2) 电子硬限位 */
        if (sv < SERVO_DEG_HARD_MIN) sv = SERVO_DEG_HARD_MIN;
        if (sv > SERVO_DEG_HARD_MAX) sv = SERVO_DEG_HARD_MAX;

        /* 3) 单拍限速：超了就只走 12°，剩下的下一拍继续 */
        const float delta = sv - s_servo[j];
        if (delta > SERVO_MAX_STEP_DEG) {
            sv = s_servo[j] + SERVO_MAX_STEP_DEG;
            ESP_LOGD(TAG, "CH%d 限速 %.1f→%.1f", j, s_servo[j], sv);
        } else if (delta < -SERVO_MAX_STEP_DEG) {
            sv = s_servo[j] - SERVO_MAX_STEP_DEG;
            ESP_LOGD(TAG, "CH%d 限速 %.1f→%.1f", j, s_servo[j], sv);
        }

        servo_write_ch(j, sv);
    }
}

void servo_get_joint_deg(float out[J_COUNT])
{
    if (out) {
        memcpy(out, s_joint, sizeof(s_joint));
    }
}

void servo_get_servo_deg(float out[J_COUNT])
{
    if (out) {
        memcpy(out, s_servo, sizeof(s_servo));
    }
}

uint32_t servo_clamp_count(void)
{
    return s_clamp_n;
}

/* ------------------------------------------------------------------ */
/* 标定                                                                */
/* ------------------------------------------------------------------ */

void servo_set_trim(int j, float deg)
{
    if (j < 0 || j >= J_COUNT) return;
    if (deg < -SERVO_TRIM_LIMIT_DEG) deg = -SERVO_TRIM_LIMIT_DEG;
    if (deg >  SERVO_TRIM_LIMIT_DEG) deg =  SERVO_TRIM_LIMIT_DEG;
    s_cal[j].trim_deg = deg;
}

float servo_get_trim(int j)
{
    return (j >= 0 && j < J_COUNT) ? s_cal[j].trim_deg : 0.0f;
}

void servo_set_dir(int j, int dir)
{
    if (j < 0 || j >= J_COUNT) return;
    const int8_t new_dir = (dir < 0) ? -1 : 1;
    const int8_t old_dir = s_cal[j].dir;
    if (new_dir == old_dir) return;

    /* 【2026-11-13】站姿不再等于装配基准后，翻转 dir 不再"天然不跳"。
     * 旧版靠「站姿时 (关节角 − 装配基准) = 0」⇒ dir 乘 0 ⇒ sv 不变。
     * 现在站姿 膝 −48° / 髋 24°，差值是 −10° / +5°，直接翻转会让 sv
     * 突变 +20° / −10°，8 路一起刷新就是整机猛地一甩。
     *
     * 改为「保持当前实际输出不变」反解新 trim：
     *     s_servo = 90 + d × dir_new + trim_new   (d = 当前关节角 − 装配基准)
     *     ⇒ trim_new = s_servo − 90 − d × dir_new
     * 用 s_servo（最近真正下发的舵机角）而不是理论 sv，可把限速/硬限位
     * 的历史也一并吸收。翻转这一步完全不驱动舵机；save 才会写进 NVS。 */
    const float d = s_joint[j] - s_ref_joint[j];
    float new_trim = s_servo[j] - 90.0f - d * (float)new_dir;
    if (new_trim < -SERVO_TRIM_LIMIT_DEG) new_trim = -SERVO_TRIM_LIMIT_DEG;
    if (new_trim >  SERVO_TRIM_LIMIT_DEG) new_trim =  SERVO_TRIM_LIMIT_DEG;

    s_cal[j].dir      = new_dir;
    s_cal[j].trim_deg = new_trim;
}

int servo_get_dir(int j)
{
    return (j >= 0 && j < J_COUNT) ? s_cal[j].dir : 1;
}

void servo_get_limit(int j, float *lo, float *hi)
{
    if (j < 0 || j >= J_COUNT) return;
    if (lo) *lo = s_cal[j].lim_min;
    if (hi) *hi = s_cal[j].lim_max;
}

esp_err_t servo_save_cal(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    int16_t trim10[J_COUNT];
    int8_t  dir[J_COUNT];
    for (int j = 0; j < J_COUNT; j++) {
        trim10[j] = (int16_t)lroundf(s_cal[j].trim_deg * 10.0f);
        dir[j]    = s_cal[j].dir;
    }

    err = nvs_set_blob(h, NVS_KEY_CAL "-t", trim10, sizeof(trim10));
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_CAL "-d", dir, sizeof(dir));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "标定值已保存: %s", esp_err_to_name(err));
    return err;
}

esp_err_t servo_load_cal(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    int16_t trim10[J_COUNT];
    int8_t  dir[J_COUNT];
    size_t len = sizeof(trim10);

    err = nvs_get_blob(h, NVS_KEY_CAL "-t", trim10, &len);
    if (err == ESP_OK && len == sizeof(trim10)) {
        for (int j = 0; j < J_COUNT; j++) {
            s_cal[j].trim_deg = (float)trim10[j] / 10.0f;
        }
    }
    len = sizeof(dir);
    esp_err_t err2 = nvs_get_blob(h, NVS_KEY_CAL "-d", dir, &len);
    if (err2 == ESP_OK && len == sizeof(dir)) {
        for (int j = 0; j < J_COUNT; j++) {
            s_cal[j].dir = (dir[j] < 0) ? -1 : 1;
        }
    }
    nvs_close(h);
    return err;
}
