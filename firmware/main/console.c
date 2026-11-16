/*
 * Spider8 命令实现（有线 REPL 与无线 HTTP 共用同一张命令表）
 *
 * 输出一律走 cli_out()：有线 stdout 照原样打印，同时把文本交给
 * 无线桥接层捕获 / 记入环形日志。不要在这里改回 printf，
 * 否则无线网页点命令会拿到空回显。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "cli_out.h"
#include "console.h"
#include "gait.h"
#include "motion.h"
#include "pca9685.h"
#include "servo.h"
#include "wireless.h"
#include "voice.h"

static const char *TAG = "cli";

/* help 表的 hint 用 config.h 的同一组宏拼字符串，
 * 避免以后放宽脉宽上限时只改校验、忘了改提示文本。 */
#define STR_(x) #x
#define STR(x)  STR_(x)

static const char *LEG_NAME[J_COUNT] = {
    "FL", "FL", "FR", "FR", "RL", "RL", "RR", "RR"
};

/* ------------------------------------------------------------------ */
/* 姿态                                                                */
/* ------------------------------------------------------------------ */

static int cmd_pose(int argc, char **argv)
{
    if (argc < 2) {
        cli_out("用法: pose <stand|crouch|lie|shake|wave>\n");
        return 1;
    }

    const char *s = argv[1];
    if      (strcmp(s, "stand")  == 0) motion_stand();
    else if (strcmp(s, "crouch") == 0) motion_pose(POSE_CROUCH);
    else if (strcmp(s, "lie")    == 0) motion_pose(POSE_LIE);
    else if (strcmp(s, "shake")  == 0) motion_pose(POSE_SHAKE);
    else if (strcmp(s, "wave")   == 0) motion_pose(POSE_WAVE);
    else {
        cli_out("未知姿态: %s\n", s);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 行走                                                                */
/* ------------------------------------------------------------------ */

static int walk_common(int argc, char **argv, gait_pattern_t pat)
{
    int speed = 100;
    if (argc >= 2) {
        speed = atoi(argv[1]);
    }
    motion_walk((uint8_t)pat, speed);
    cli_out("请求 %s，速度 %d%%（先平稳收回站姿，再起步）\n",
            gait_pattern_name((uint8_t)pat), speed);
    return 0;
}

static int cmd_walk(int argc, char **argv) { return walk_common(argc, argv, GAIT_PATTERN_WALK); }
static int cmd_trot(int argc, char **argv) { return walk_common(argc, argv, GAIT_PATTERN_TROT); }

static int cmd_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    motion_stand();      /* 顺便把 turn 归零，不把转向带进下一次起步 */
    return 0;
}

/* 原地左/右转：2-DOF 腿不能横向跨步，转向只能靠左右步幅差动。
 * 之前只有 turn 这个「持续参数」，且 speed=0 时整条 stride 被乘成 0，
 * 原地根本转不了；gait.c 已把前进分量与自转分量拆开相加。 */
static int cmd_spin_left(int argc, char **argv)
{
    (void)argc; (void)argv;
    motion_spin(-1);
    cli_out("原地左转（差速 %d%%）；straight 回正直行、stop 站定\n",
            -GAIT_SPIN_TURN_PCT);
    return 0;
}

static int cmd_spin_right(int argc, char **argv)
{
    (void)argc; (void)argv;
    motion_spin(1);
    cli_out("原地右转（差速 +%d%%）；straight 回正直行、stop 站定\n",
            GAIT_SPIN_TURN_PCT);
    return 0;
}

static int cmd_straight(int argc, char **argv)
{
    (void)argc; (void)argv;
    motion_straight();
    cli_out("转向回正，直行前进\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 参数                                                                */
/* ------------------------------------------------------------------ */

static int cmd_speed(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: speed <0-100>\n"); return 1; }
    motion_set_speed(atoi(argv[1]));
    cli_out("速度 = %d%%\n", motion_params()->speed_pct);
    return 0;
}

static int cmd_turn(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: turn <-100..100>\n"); return 1; }
    motion_set_turn(atoi(argv[1]));
    cli_out("转向 = %d%%\n", motion_params()->turn_pct);
    return 0;
}

static int cmd_period(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: period <800-3000> ms\n"); return 1; }
    motion_set_period(atoi(argv[1]));
    cli_out("周期 = %ums\n", motion_params()->period_ms);
    return 0;
}

static int cmd_stride(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: stride <8-30> mm\n"); return 1; }
    motion_set_stride(strtof(argv[1], NULL));
    cli_out("步幅半幅 = %.1fmm\n", motion_params()->stride_mm);
    return 0;
}

static int cmd_lift(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: lift <6-26> mm\n"); return 1; }
    motion_set_lift(strtof(argv[1], NULL));
    cli_out("抬脚 = %.1fmm\n", motion_params()->lift_mm);
    return 0;
}

static int cmd_duty(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: duty <10-45> %%\n"); return 1; }
    motion_set_duty(atoi(argv[1]));
    cli_out("占空比 = %d%%\n", motion_params()->duty_pct);
    return 0;
}

static int cmd_posedur(int argc, char **argv)
{
    if (argc < 2) {
        cli_out("姿态过渡时长 = %lums（站/蹲/趴/握手的缓动时间）\n",
               (unsigned long)motion_pose_trans_ms());
        cli_out("用法: posedur <%d-%d>   越大越慢越柔；0 = 恢复默认 %dms\n",
               POSE_TRANS_MS_MIN, POSE_TRANS_MS_MAX, POSE_TRANS_MS_DEFAULT);
        return 0;
    }
    int ms = atoi(argv[1]);
    if (ms == 0) ms = POSE_TRANS_MS_DEFAULT;
    if (ms < POSE_TRANS_MS_MIN || ms > POSE_TRANS_MS_MAX) {
        cli_out("范围是 %d~%dms\n", POSE_TRANS_MS_MIN, POSE_TRANS_MS_MAX);
        return 1;
    }
    motion_set_pose_trans(ms);
    cli_out("姿态过渡时长 = %lums\n", (unsigned long)motion_pose_trans_ms());
    return 0;
}

/* 换动作时「收脚回站姿 / 起步淡入」的时长。与 posedur 分开：
 * posedur 管蹲趴握手的机身升降，fadedur 管步幅收放。 */
static int cmd_fadedur(int argc, char **argv)
{
    if (argc < 2) {
        cli_out("收脚/起步淡入时长 = %lums\n",
                (unsigned long)motion_fade_trans_ms());
        cli_out("用法: fadedur <%d-%d>   越大越慢越柔；0 = 恢复默认 %dms\n",
                GAIT_FADE_MS_MIN, GAIT_FADE_MS_MAX, GAIT_FADE_MS_DEFAULT);
        return 0;
    }
    int ms = atoi(argv[1]);
    if (ms == 0) ms = GAIT_FADE_MS_DEFAULT;
    if (ms < GAIT_FADE_MS_MIN || ms > GAIT_FADE_MS_MAX) {
        cli_out("范围是 %d~%dms\n", GAIT_FADE_MS_MIN, GAIT_FADE_MS_MAX);
        return 1;
    }
    motion_set_fade_trans(ms);
    cli_out("收脚/起步淡入时长 = %lums（只影响下一次切换）\n",
            (unsigned long)motion_fade_trans_ms());
    return 0;
}

/* 站姿停留：回到中间态站姿后，等「输出追平 + 舵机停稳」再执行下一个动作。
 * 与 fadedur 分开：fadedur 管收脚/起步，standhold 管站稳的等待窗口。 */
static int cmd_standhold(int argc, char **argv)
{
    if (argc < 2) {
        cli_out("站姿停留 = %lums（回站姿后等真正站稳，再切下一个动作）\n",
                (unsigned long)motion_settle_trans_ms());
        cli_out("用法: standhold <%d-%d>  越大越稳；0 = 恢复默认 %dms\n",
                GAIT_SETTLE_MS_MIN, GAIT_SETTLE_MS_MAX, GAIT_SETTLE_MS_DEFAULT);
        cli_out("放行条件：停留够时间，且 8 路关节角连续 %d 拍收敛到 %.1f° 内。\n",
                GAIT_SETTLE_OK_BEATS, GAIT_SETTLE_TOL_DEG);
        return 0;
    }
    int ms = atoi(argv[1]);
    if (ms == 0) ms = GAIT_SETTLE_MS_DEFAULT;
    if (ms < GAIT_SETTLE_MS_MIN || ms > GAIT_SETTLE_MS_MAX) {
        cli_out("范围是 %d~%dms\n", GAIT_SETTLE_MS_MIN, GAIT_SETTLE_MS_MAX);
        return 1;
    }
    motion_set_settle_trans(ms);
    cli_out("站姿停留 = %lums（只影响下一次切换）\n",
            (unsigned long)motion_settle_trans_ms());
    return 0;
}

/* 站立屈膝角。装配基准 -38° 是 trim 零点，不能改；这里只改「目标站姿」。
 * 髋角不单独给，永远由 gait_hip_for_knee() 反解（两连杆等长 ⇒ 髋 = −膝/2）。 */
static int cmd_standknee(int argc, char **argv)
{
    const gait_params_t *p = motion_params();

    if (argc < 2) {
        cli_out("站立屈膝角 = %.1f° → 髋 %.1f°，髋轴离地 %.1fmm\n",
                p->stand_knee_deg, gait_hip_for_knee(p->stand_knee_deg),
                gait_height_for_knee(p->stand_knee_deg));
        cli_out("用法: standknee <%.0f..%.0f>  负值越大腿越弯；0 = 恢复默认 %.0f°\n",
                GAIT_STAND_KNEE_MIN, GAIT_STAND_KNEE_MAX,
                GAIT_DEFAULT_STAND_KNEE_DEG);
        cli_out("装配基准 %.0f° 不变 ⇒ trim/dir 无需重标；髋自动 = −膝/2。\n",
                GAIT_STAND_KNEE_DEG);
        return 0;
    }

    /* 严格解析：strtof 打错会返回 0，不能让它悄悄把腿掰直 */
    char *end = NULL;
    float deg = strtof(argv[1], &end);
    if (end == argv[1] || (end && *end != '\0')) {
        cli_out("角度必须是数字，例如 standknee -48\n");
        return 1;
    }
    if (deg == 0.0f) deg = GAIT_DEFAULT_STAND_KNEE_DEG;
    if (deg < GAIT_STAND_KNEE_MIN || deg > GAIT_STAND_KNEE_MAX) {
        cli_out("范围是 %.0f~%.0f°（趴下最深 −95°，再深会顶到软限位）\n",
                GAIT_STAND_KNEE_MIN, GAIT_STAND_KNEE_MAX);
        return 1;
    }

    motion_set_stand_knee(deg);
    const gait_params_t *q = motion_params();
    cli_out("站立屈膝角 = %.1f° → 髋 %.1f°，髋轴离地 %.1fmm（%lums 平滑过渡）\n",
            q->stand_knee_deg, gait_hip_for_knee(q->stand_knee_deg),
            gait_height_for_knee(q->stand_knee_deg),
            (unsigned long)motion_pose_trans_ms());
    return 0;
}

/* ------------------------------------------------------------------ */
/* 标定                                                                */
/* ------------------------------------------------------------------ */

/* 严格解析关节号。注意不能用 atoi：atoi("foo") == 0，会把打错的参数
 * 悄悄当成 CH0 去驱动。 */
static bool parse_joint(const char *s, int *out);

static int cmd_cal(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        motion_stand();
        cli_out("退出标定模式\n");
        return 0;
    }
    if (argc < 3) {
        cli_out("用法: cal <0-7> <关节角>   |   cal off\n");
        cli_out("  关节角：髋 0=竖直向下，+向前；膝 0=伸直，负向后折\n");
        return 1;
    }

    int j = -1;
    if (!parse_joint(argv[1], &j)) {
        cli_out("关节号必须是 0~7（收到的是 \"%s\"）\n", argv[1]);
        return 1;
    }
    float deg = strtof(argv[2], NULL);
    motion_manual(j, deg);
    cli_out("CH%d %s %s ← %.1f°（舵机 %.1f°）\n",
           j, LEG_NAME[j], JOINT_IS_HIP(j) ? "髋" : "膝",
           deg, servo_joint_to_servo(j, deg));
    return 0;
}

static int cmd_trim(int argc, char **argv)
{
    /* 50 与 config.h 的 SERVO_TRIM_LIMIT_DEG 一致（360° 舵机换算后需要 ±43° 左右） */
    if (argc < 3) { cli_out("用法: trim <0-7> <微调角 -50..50>\n"); return 1; }
    int j = -1;
    if (!parse_joint(argv[1], &j)) { cli_out("关节号必须是 0~7\n"); return 1; }
    servo_set_trim(j, strtof(argv[2], NULL));
    cli_out("CH%d trim = %.1f°\n", j, servo_get_trim(j));
    return 0;
}

static int cmd_dir(int argc, char **argv)
{
    if (argc < 3) { cli_out("用法: dir <0-7> <1|-1>\n"); return 1; }
    int j = -1;
    if (!parse_joint(argv[1], &j)) { cli_out("关节号必须是 0~7\n"); return 1; }
    servo_set_dir(j, atoi(argv[2]));
    cli_out("CH%d dir = %d\n", j, servo_get_dir(j));
    return 0;
}

/* 手机 App 的 trim 标定页专用：一次只读一路，回显约 15 字节。
 * 背景：BLE 回话是「严格单包」，按实际 ATT_MTU 截断（安卓默认 23 时净荷仅 20B），
 * 而 status 的完整表格近千字节，手机永远只能收到开头几行、拿不到 8 路 trim/dir。
 * trimget 让 App 依次查 0~7，任何 MTU 都塞得下，也不必再动 BLE 分包逻辑。 */
static int cmd_trimget(int argc, char **argv)
{
    if (argc < 2) { cli_out("用法: trimget <0-7>\n"); return 1; }
    int j = -1;
    if (!parse_joint(argv[1], &j)) { cli_out("关节号必须是 0~7\n"); return 1; }
    cli_out("T%d=%.1f/%d\n", j, servo_get_trim(j), servo_get_dir(j));
    return 0;
}

static int cmd_save(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = servo_save_cal();
    cli_out("%s\n", (err == ESP_OK) ? "标定值已写入 NVS" : "写入失败");
    return (err == ESP_OK) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* 电气验证：直接输出脉宽                                              */
/* ------------------------------------------------------------------ */

/* 严格解析通道号：只接受纯数字的 0~7。
 * 不能用 atoi 直接判 —— atoi("foo") 返回 0，会把打错的参数悄悄当成 CH0，
 * 然后真的去驱动那一路舵机。 */
static bool parse_joint(const char *s, int *out)
{
    if (s == NULL || s[0] < '0' || s[0] > '9') {
        return false;
    }
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v >= J_COUNT) {
        return false;
    }
    *out = (int)v;
    return true;
}

/* 把"脉宽"翻译成普通人能对上号的东西：占空比 + 平均电压。
 * 没有示波器时，万用表直流档量到的就是这个平均电压。
 * 平均电压按输出高电平算：LEDC = ESP32 GPIO 3.3V；
 * PCA9685 = 芯片 VCC（本项目接 ESP32 3V3），不是 V+ 端子的 5V。 */
static void pwm_print_one(int j, int us)
{
    char lbl[16];
    servo_out_label(j, lbl, sizeof(lbl));
    /* 两种后端的输出高电平都是 3.3V：
     *   LEDC     → ESP32 GPIO 电平 3.3V
     *   PCA9685  → 芯片 VCC（本项目固定接 ESP32 3V3），不是 V+ 端子的 5V
     * V+ 的 5V 只给舵机动力，不决定信号幅值。曾误按 5V 换算，导致打印
     * 0.375V / 0.625V 与实测 0.248V / 0.413V 对不上，白白误导排查。 */
    const float vhigh = 3.3f;
    cli_out("CH%d %-5s %4dus  占空比 %5.2f%%  万用表应读 ≈%.3fV\n",
           j, lbl, us, (float)us * 100.0f / 20000.0f,
           vhigh * (float)us / 20000.0f);
}

static int cmd_pwm(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        servo_raw_exit();
        motion_stand();
        cli_out("退出 PWM 测试，回到站姿\n");
        return 0;
    }

    if (argc < 3) {
        cli_out("用法: pwm <0-7|all> <%d-%d>us   |   pwm off\n",
               SERVO_US_CMD_MIN, SERVO_US_CMD_MAX);
        cli_out("  例：pwm 0 1500   单通道输出 1.5ms 中位脉宽\n");
        cli_out("      pwm 0 2900   测 360° 大行程舵机的高脉宽端点\n");
        cli_out("      pwm all 500  8 路同时输出 0.5ms，逐个量电压最省事\n");
        return 1;
    }

    const int us = atoi(argv[2]);
    if (us < SERVO_US_CMD_MIN || us > SERVO_US_CMD_MAX) {
        cli_out("脉宽必须是 %d~%dus（裸脉宽测试范围；正常姿态另受 0~180° 硬限位约束）\n",
               SERVO_US_CMD_MIN, SERVO_US_CMD_MAX);
        return 1;
    }

    /* 先校验参数再进 raw 模式 —— 否则参数打错会让机器人一直冻结在测试模式 */
    const bool all_ch = (strcmp(argv[1], "all") == 0);
    int j = -1;
    if (!all_ch && !parse_joint(argv[1], &j)) {
        cli_out("关节号必须是 0~7 或 all\n");
        return 1;
    }

    servo_raw_enter();

    if (all_ch) {
        for (int k = 0; k < J_COUNT; k++) {
            servo_raw_us(k, us);
        }
        cli_out("8 路同时输出 %dus（占空比 %.2f%%）\n", us, (float)us * 100.0f / 20000.0f);
    } else {
        servo_raw_us(j, us);
        pwm_print_one(j, us);
    }

    cli_out("⚠ PWM 测试模式：步态/姿态已暂停，pwm off 恢复\n");
    return 0;
}

static void pwm_emit(int target, int us)
{
    if (target < 0) {
        for (int j = 0; j < J_COUNT; j++) {
            servo_raw_us(j, us);
        }
    } else {
        servo_raw_us(target, us);
    }
}

static int cmd_pwmscan(int argc, char **argv)
{
    int target = -1;                        /* -1 = 8 路一起扫 */

    if (argc >= 2 && strcmp(argv[1], "all") != 0) {
        if (!parse_joint(argv[1], &target)) {
            cli_out("用法: pwmscan <0-7|all>\n");
            return 1;
        }
    }

    const int lo = SERVO_US_CMD_MIN, hi = SERVO_US_CMD_MAX, step = 25, dt_ms = 20, cycles = 2;
    const float secs = 2.0f * ((hi - lo) / step) * dt_ms * cycles / 1000.0f;

    servo_raw_enter();

    if (target < 0) {
        cli_out("8 路同时呼吸：%d→%dµs→%dµs，%d 个来回，约 %.1f 秒\n",
               lo, hi, lo, cycles, secs);
        cli_out("  肉眼应看到 8 个 LED 同步由暗到亮再变暗\n");
    } else {
        char lbl[16];
        servo_out_label(target, lbl, sizeof(lbl));
        cli_out("CH%d(%s) 呼吸：%d→%dµs，%d 个来回，约 %.1f 秒\n",
               target, lbl, lo, hi, cycles, secs);
        cli_out("  只该这一路亮，其余 7 个脚必须完全不亮\n");
    }

    for (int c = 0; c < cycles; c++) {
        for (int us = lo; us <= hi; us += step) {
            pwm_emit(target, us);
            vTaskDelay(pdMS_TO_TICKS(dt_ms));
        }
        for (int us = hi; us >= lo; us -= step) {
            pwm_emit(target, us);
            vTaskDelay(pdMS_TO_TICKS(dt_ms));
        }
    }

    /* 收尾：给中位脉宽，退出测试模式，姿态平滑回位 */
    pwm_emit(target, 1500);
    servo_raw_exit();
    motion_stand();
    cli_out("扫描结束，已回站姿\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 状态 / 演示                                                         */
/* ------------------------------------------------------------------ */

/* PCA9685 寄存器诊断：定位「I2C 通、status 正常，但输出脚恒 3.3V / 全 0」 */
static int cmd_pcadiag(int argc, char **argv)
{
    (void)argc; (void)argv;
    return (pca9685_diag() == ESP_OK) ? 0 : 1;
}

/* 无线诊断：手机/电脑搜不到热点时，插 USB 线敲这条命令即可看到
 * Wi-Fi 实际状态 + 开机时的系统日志（含 wireless_init 的报错）。 */
static int cmd_wifidiag(int argc, char **argv)
{
    (void)argc; (void)argv;

    wireless_diag();

    /* 静态缓冲，避免把 8KB 压到 REPL 任务栈上 */
    static char log[8192];
    (void)cli_log_read(0, log, sizeof(log));
    cli_out("\r\n---- 系统日志（环形缓冲，含开机时的无线报错）----\r\n");
    cli_out("%s", log);
    cli_out("\r\n---- 日志结束 ----\r\n");
    return 0;
}

/* 配错家里 WiFi 密码时的有线恢复路径：只清配网命名空间，不动 trim。 */
static int cmd_wificlear(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_err_t e = wireless_clear_sta_config();
    if (e == ESP_OK) {
        cli_out("已清除家里 WiFi 配网。请断电重启板子，之后只开热点 spider8 / 192.168.4.1。\r\n");
        cli_out("舵机 trim 不受影响。\r\n");
        return 0;
    }
    cli_out("清除失败: %s\r\n", esp_err_to_name(e));
    return 1;
}

/* 网页配网走不通时（如本机 VPN 抢路由），插线直接写家里 WiFi。
 * 严格校验参数个数，不用 atoi 之类宽松判法。 */
static int cmd_wifiset(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        cli_out("用法: wifiset <家里WiFi名> <密码>\r\n");
        cli_out("      开放网络（无密码）: wifiset <家里WiFi名> -\r\n");
        cli_out("      密码含空格时用双引号包住整个密码\r\n");
        return 1;
    }
    const char *pass = (argc == 3) ? argv[2] : "-";
    const esp_err_t e = wireless_set_sta_config(argv[1], pass);
    return (e == ESP_OK) ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;

    const gait_params_t *p = motion_params();
    float jd[J_COUNT], sd[J_COUNT];
    motion_get_out(jd);
    servo_get_servo_deg(sd);

    if (servo_raw_active()) {
        cli_out("\n⚠ PWM 电气测试模式生效中 —— 步态与姿态暂停，pwm off 退出\n");
    }

    cli_out("\n模式 %s / 姿态 %s / 步态 %s\n",
           motion_mode_name(), motion_pose_name(motion_current_pose()),
           gait_pattern_name(p->pattern));
    cli_out("周期 %ums  占空 %d%%  步幅 %.1fmm  抬脚 %.1fmm  速度 %d%%  转向 %d%%\n",
           p->period_ms, p->duty_pct, p->stride_mm, p->lift_mm,
           p->speed_pct, p->turn_pct);
    cli_out("姿态过渡 %lums（posedur 可调）\n",
           (unsigned long)motion_pose_trans_ms());
    cli_out("动作切换 %s / 目标 %s / 淡入淡出 %lums / 站稳停留 %lums（fadedur、standhold 可调）\n",
           motion_trans_name(), motion_pending_name(),
           (unsigned long)motion_fade_trans_ms(),
           (unsigned long)motion_settle_trans_ms());
    cli_out("站姿 膝 %.1f° / 髋 %.1f°  髋轴离地 %.1fmm（standknee 可调，装配基准 %.0f° 不变）\n",
           p->stand_knee_deg, gait_hip_for_knee(p->stand_knee_deg),
           gait_height_for_knee(p->stand_knee_deg), GAIT_STAND_KNEE_DEG);
    cli_out("标称前进 = %.1fmm/周期 ≈ %.1fmm/s\n",
           2.0f * p->stride_mm * (p->speed_pct / 100.0f),
           2.0f * p->stride_mm * (p->speed_pct / 100.0f) * 1000.0f / p->period_ms);

    /* 舵机跟不跟得上 —— 这是开环舵机最容易翻车的地方 */
    {
        float rate = 0.0f, per_cycle = 0.0f;
        gait_estimate(p, &rate, &per_cycle);
        const float margin = (SERVO_MAX_RATE_DPS - rate) / SERVO_MAX_RATE_DPS * 100.0f;

        cli_out("峰值角速度 ≈ %.0f°/s（MG90S %.0f°/s，余量 %.0f%%）%s\n",
               rate, SERVO_MAX_RATE_DPS, margin,
               (rate > SERVO_MAX_RATE_DPS) ? "  ✗ 超限，请加长周期或减小抬脚" :
               (margin < 15.0f)             ? "  ⚠ 余量偏小" : "  ✓");
    }

    cli_out("\nCH 腿  关节  关节角   舵机角   dir  trim\n");
    for (int j = 0; j < J_COUNT; j++) {
        cli_out("%2d %s  %s  %7.1f° %7.1f°  %+d  %+.1f°\n",
               j, LEG_NAME[j], JOINT_IS_HIP(j) ? "髋" : "膝",
               jd[j], sd[j], servo_get_dir(j), servo_get_trim(j));
    }

    uint32_t clamp_n = servo_clamp_count();
    if (clamp_n > 0) {
        cli_out("\n⚠ 软限位触发 %lu 次 —— 步幅/抬脚超了机构行程\n",
               (unsigned long)clamp_n);
    }

    uint32_t mh = 0, sh = 0;
    motion_diag(&mh, &sh);
    cli_out("\n堆 free=%lu min=%lu（含 PSRAM，不能用来判断语音能否共存）\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());
    /* I0 语音集成体检：esp-sr 的 AFE / WakeNet / MultiNet 只能吃片内 SRAM，
     * 外挂 PSRAM 再大也不算。这里把两者分开列，避免再被 8.5MB 误导。 */
    cli_out("片内SRAM free=%lu min=%lu 最大连续块=%lu\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cli_out("PSRAM free=%lu min=%lu\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cli_out("栈余量(字) motion=%lu servo_out=%lu\n",
           (unsigned long)mh, (unsigned long)sh);
    cli_out("运行 %lus\n", (unsigned long)(esp_log_timestamp() / 1000));
    return 0;
}

/* I0 语音集成体检：一条能在「关掉 Wi-Fi、只有串口/BLE」时读到的短命令。
 * status 的 8 路表近千字节，BLE 严格单包收不全；meminfo 固定一行 <120B，
 * 手机 App 命令框、BLE 写特征、语音模拟通道都能直接读回。 */
static int cmd_meminfo(int argc, char **argv)
{
    (void)argc; (void)argv;
#ifdef CONFIG_SPIDER8_WIFI_ENABLED
    const char *wifi = "开";
#else
    const char *wifi = "关";
#endif
    cli_out("MEM 片内free=%lu min=%lu 最大块=%lu | PSRAMfree=%lu min=%lu | WiFi=%s\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
            wifi);
    return 0;
}

static int cmd_demo(int argc, char **argv)
{
    (void)argc; (void)argv;
    cli_out("演示开始（约 9 秒）\n");

    motion_pose(POSE_CROUCH); vTaskDelay(pdMS_TO_TICKS(1600));
    motion_stand();           vTaskDelay(pdMS_TO_TICKS(1200));
    motion_pose(POSE_SHAKE);  vTaskDelay(pdMS_TO_TICKS(2200));
    motion_pose(POSE_WAVE);   vTaskDelay(pdMS_TO_TICKS(3000));
    motion_stand();

    cli_out("演示结束\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 注册                                                                */
/* ------------------------------------------------------------------ */

static void register_all(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "pose",   .help = "切换姿态", .hint = "stand|crouch|lie|shake|wave", .func = cmd_pose },
        { .command = "walk",   .help = "波步前进（四拍，最稳）", .hint = "[速度0-100]", .func = cmd_walk },
        { .command = "trot",   .help = "小跑（对角，快）",       .hint = "[速度0-100]", .func = cmd_trot },
        { .command = "stop",   .help = "停止并站定",             .func = cmd_stop },
        { .command = "left",   .help = "原地左转（差速）",       .func = cmd_spin_left },
        { .command = "right",  .help = "原地右转（差速）",       .func = cmd_spin_right },
        { .command = "straight",.help = "转向回正并直行",        .func = cmd_straight },
        { .command = "speed",  .help = "前进速度",               .hint = "<0-100>", .func = cmd_speed },
        { .command = "turn",   .help = "差速转向",               .hint = "<-100..100>", .func = cmd_turn },
        { .command = "period", .help = "步态周期",               .hint = "<800-3000>ms", .func = cmd_period },
        { .command = "stride", .help = "步幅半幅",               .hint = "<8-30>mm", .func = cmd_stride },
        { .command = "lift",   .help = "抬脚高度",               .hint = "<6-26>mm", .func = cmd_lift },
        { .command = "duty",   .help = "摆动相占空比",           .hint = "<10-45>%", .func = cmd_duty },
        { .command = "posedur",.help = "姿态过渡时长（越大越慢越柔）", .hint = "<200-3000>ms", .func = cmd_posedur },
        { .command = "fadedur",.help = "收脚/起步淡入时长",     .hint = "<200-2000>ms", .func = cmd_fadedur },
        { .command = "standhold",.help = "站姿停留时长（等站稳再切动作）",
          .hint = "<100-2000>ms | 0=默认", .func = cmd_standhold },
        { .command = "standknee",.help = "站立屈膝角（负值越大腿越弯，髋自动跟）",
          .hint = "<-70..-30>° | 0=默认", .func = cmd_standknee },
        { .command = "cal",    .help = "单关节点动标定",         .hint = "<0-7> <角度> | cal off", .func = cmd_cal },
        { .command = "trim",   .help = "舵机微调角",             .hint = "<0-7> <-50..50>", .func = cmd_trim },
        { .command = "dir",    .help = "舵机方向翻转",           .hint = "<0-7> <1|-1>", .func = cmd_dir },
        { .command = "trimget",.help = "读单路 trim/dir（手机标定页）", .hint = "<0-7>", .func = cmd_trimget },
        { .command = "save",   .help = "标定值写入 NVS",         .func = cmd_save },
        { .command = "pwm",    .help = "电气验证：单通道固定脉宽",
          .hint = "<0-7|all> <" STR(SERVO_US_CMD_MIN) "-" STR(SERVO_US_CMD_MAX) ">us | pwm off",
          .func = cmd_pwm },
        { .command = "pwmscan",.help = "电气验证：呼吸扫描脉宽",   .hint = "<0-7|all>", .func = cmd_pwmscan },
        { .command = "pcadiag",.help = "读回 PCA9685 寄存器诊断输出", .func = cmd_pcadiag },
        { .command = "status", .help = "打印当前状态",           .func = cmd_status },
        { .command = "meminfo",.help = "内存体检：片内SRAM/PSRAM 余量（语音集成 I0）", .func = cmd_meminfo },
        { .command = "voice",  .help = "语音前端：状态/词表/识别开关/动作放行/模拟播报",
          .hint = "[status|list|on|off|exec on|off|mute on|off|sim <1-25>|say <0-22>|gain <12-20>]",
          .func = voice_cli },
        { .command = "wifidiag", .help = "无线诊断：热点状态 + 开机日志", .func = cmd_wifidiag },
        { .command = "wificlear", .help = "清除家里WiFi配网，重启回纯热点", .func = cmd_wificlear },
        { .command = "wifiset", .help = "串口直接写家里WiFi并重启",
          .hint = "<ssid> <pass>", .func = cmd_wifiset },
        { .command = "demo",   .help = "动作演示：蹲→站→握手→挥手", .func = cmd_demo },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "命令 %s 注册失败: %s", cmds[i].command, esp_err_to_name(err));
        }
    }
}

esp_err_t console_init(void)
{
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "spider8>";
    repl_cfg.max_cmdline_length = 128;
    repl_cfg.task_stack_size = CONSOLE_TASK_STACK;
    repl_cfg.task_priority = CONSOLE_TASK_PRIO;

    /* 命令行挂在 ESP32-S3 的原生 USB Serial/JTAG 上，不是 UART0。
     * 本板 UART0 只引到"UART"口（CH340），而那一路驱动在这台机器上不可用；
     * 唯一能用的通道就是"USB"口的 USB Serial/JTAG。 */
    esp_console_dev_usb_serial_jtag_config_t usb_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t     err = esp_console_new_repl_usb_serial_jtag(&usb_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL 创建失败: %s", esp_err_to_name(err));
        /* USB 口不可用时仍要把命令表建起来，否则无线入口也一起废掉。
         * esp_console_run() 依赖 esp_console_init()；IDF 5.5 起它要收一个配置结构体。 */
        esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();
        (void)esp_console_init(&cfg);
        register_all();
        return err;
    }

    err = esp_console_register_help_command();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "help 命令注册失败: %s", esp_err_to_name(err));
    }

    register_all();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "命令行就绪，输入 help 查看命令");
    return ESP_OK;
}
