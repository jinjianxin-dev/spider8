/*
 * Spider8 四足机器狗 —— 全局配置
 *
 * 这里只放"装机时可能要改"的常量：引脚、机械尺寸、舵机映射基准、软限位。
 * 算法参数（周期/步幅/抬脚）走串口命令在线调整，不在这里硬编码。
 */
#ifndef SPIDER8_CONFIG_H
#define SPIDER8_CONFIG_H

#include <stdint.h>

/* ============================== 版本 ============================== */
#define FW_VERSION              "0.2.0"

/* ============================ 关节编号 ============================
 * 8 个关节，顺序与 LEDC 通道号、引脚表一一对应：
 *   CH0 CH1 | CH2 CH3 | CH4 CH5 | CH6 CH7
 *   FL髋 FL膝| FR髋 FR膝| RL髋 RL膝| RR髋 RR膝
 * 腿索引 leg = joint / 2，髋 = joint & ~1，膝 = joint | 1
 * ================================================================== */
#define LEG_COUNT               4
#define J_COUNT                 8

#define LEG_FL                  0
#define LEG_FR                  1
#define LEG_RL                  2
#define LEG_RR                  3

#define J_FL_HIP                0
#define J_FL_KNEE               1
#define J_FR_HIP                2
#define J_FR_KNEE               3
#define J_RL_HIP                4
#define J_RL_KNEE               5
#define J_RR_HIP                6
#define J_RR_KNEE               7

#define JOINT_LEG(j)            ((j) >> 1)
#define JOINT_IS_HIP(j)         (((j) & 1) == 0)

/* ============================ 舵机引脚 ============================
 * 与 docs/02-设计方案.md §2.1 一致。
 * 注意：M0-1 曾用 GPIO4 做心跳 LED，现已让给舵机 CH0；心跳改为纯日志输出。
 * 这 8 个脚在 ESP32-S3 上均不与 Flash/PSRAM/USB 冲突。
 * ================================================================== */
#define SERVO_PIN_FL_HIP        4
#define SERVO_PIN_FL_KNEE       5
#define SERVO_PIN_FR_HIP        6
#define SERVO_PIN_FR_KNEE       7
#define SERVO_PIN_RL_HIP        15
#define SERVO_PIN_RL_KNEE       16
#define SERVO_PIN_RR_HIP        17
#define SERVO_PIN_RR_KNEE       18

/* ============================ 机械尺寸 ============================
 * 乐高 Technic 拼装，先按 55mm 建模；实物量出来差多少就改这里，
 * 所有运动学、站立高度、蹲下高度都会自动跟着变。
 * 两条腿等长时有个漂亮性质：髋角恒等于 -膝角/2（脚永远在髋点正下方）。
 * ================================================================== */
#define GAIT_THIGH_MM           55.0f   /* 大腿 L1 */
#define GAIT_SHIN_MM            55.0f   /* 小腿 L2 */

/* ==================== 装配基准 与 默认站姿（两者必须分开） ========
 * 【装配基准 · 别改】GAIT_STAND_KNEE_DEG = -38°
 *   装腿时把舵机停在 1500µs + trim，腿摆成「膝 -38° / 髋 +19°」。
 *   servo.c 的 s_ref_joint 用它换算关节角 → 舵机角：
 *       sv = 90 + (目标关节角 − 装配基准) × dir + trim
 *   所以它是 trim/dir 标定的零点。改它 = 8 个 trim 全部作废、且
 *   「目标角 = 基准角」时 sv 仍等于 90+trim，舵机纹丝不动（腿不会弯）。
 *
 * 【默认站姿 · 可调】GAIT_DEFAULT_STAND_KNEE_DEG = -48°（2026-11-13）
 *   上电 / 站立 / 步态用的机身高度基准。相对装配基准多折 10°，
 *   髋由 IK 自动反解为 +24°（髋 = −膝/2），髋轴离地 100.5mm（原 104.0mm）。
 *   两者拆开后：舵机相对装配零位多走「膝 −10°、髋 +5°」，腿真的弯下去，
 *   trim 一个都不用重标，NVS 也不动。
 *   运行时可用串口 `standknee <-70..-30>` 现场微调，不用重烧。
 *
 * 【为什么髋不能也弯 10°】两连杆等长时脚要落在髋点正下方，
 *   髋角恒等于 −膝角/2。硬让髋 +10° 会让四只脚整体前移 8.7mm，
 *   重心跑出支撑面、站着往后栽。这也是 POSE 过渡只插值膝角的原因。
 * ================================================================== */
#define GAIT_STAND_KNEE_DEG           (-38.0f)  /* 装配基准 = trim 零点 */
#define GAIT_DEFAULT_STAND_KNEE_DEG   (-48.0f)  /* 默认站姿，可在线改 */
#define GAIT_STAND_KNEE_MIN           (-70.0f)  /* standknee 下限（趴下有 −95° 兜底） */
#define GAIT_STAND_KNEE_MAX           (-30.0f)  /* standknee 上限，再直就失去缓冲 */

/* 蹲/趴是相对「当前站姿」的膝角增量：站姿改了动作幅度跟着走，
 * 不会出现「站得比蹲还低」。下限 −95°：膝软限位 −100°，
 * 趴到 -95° 时髋已 47.5°，不能再深。默认 -48° → 蹲 -70° / 趴 -95°。 */
#define POSE_CROUCH_DK      (-22.0f)
#define POSE_LIE_DK         (-47.0f)
#define POSE_KNEE_FLOOR     (-95.0f)

/* ============================== 时序 ============================== */
#define GAIT_TICK_MS            20      /* 步态/动作控制周期 */
#define MOTION_TASK_STACK       4096
#define MOTION_TASK_PRIO        5
#define SERVO_TASK_STACK        3072
#define SERVO_TASK_PRIO         7       /* 高于 motion：PWM 刷新不能被打断太久 */
#define CONSOLE_TASK_STACK      4096
#define CONSOLE_TASK_PRIO       3

/* ============================ 舵机电气 ============================
 * LEDC 后端用；PCA9685 后端只有 SERVO_PERIOD_US（20ms 周期）是共用的。 */
#define SERVO_PWM_FREQ_HZ       50
#define SERVO_PWM_RES           LEDC_TIMER_14_BIT
#define SERVO_PWM_FULL          16384u  /* 2^14，占空比满量程计数 */
#define SERVO_PERIOD_US         20000u  /* 50Hz → 20ms */

/* ====================== PCA9685 后端（I2C） ======================
 * 舵机信号线全部接到 PCA9685 的 0~7 通道，ESP32 只占 SDA/SCL 两根线。
 * V+ 接 5V/3A 适配器（舵机电源），VCC 接 ESP32 3V3（芯片逻辑电源），
 * GND 三方共地：适配器、ESP32、PCA9685。
 * 注意：PCA9685 是 12bit，1 计数 ≈ 4.88µs ≈ 0.44°，标定精度比 LEDC 粗。 */
#define SERVO_PCA_ADDR          0x40    /* 板上地址跳线全断开时的默认值 */
#define SERVO_PCA_SDA_GPIO      8       /* 两个脚都不与 Flash/PSRAM/Strap 冲突 */
#define SERVO_PCA_SCL_GPIO      9
#define SERVO_PCA_I2C_HZ        400000  /* 400kHz 快速模式 */

/* 舵机内部角（0~180°）→ 脉宽：us = us_mid + (deg - 90) * us_span / 180
 *
 * 【本机实测 · 2026-11-11】8 路是 360° 位置舵机，波段 500~2900µs ↔ 0~360°，
 * 即 1° 物理角 = 2400/360 = 6.667µs。要让「1° 关节指令 = 1° 物理转角」，
 * us_span 必须是 180 × 6.667 = 1200（通用公式 = 实测跨度 ÷ 2）。
 *   us_span=2000（旧值，按普通 180° 舵机）→ 1° 指令实际走 1.667°，
 *   步态 IK、软限位、12°/拍限速全部放大 1.667 倍（物理 1000°/s > 600°/s）。
 *   us_span=1200（现值）           → 0~180° 内部刻度 ↔ 900~2100µs，
 *   相对 1500µs 的物理转角正好 ±90°，与膝/髋软限位设计一致；
 *   12°/20ms = 600°/s 也正好落回 MG90S 标称极限。
 *
 * us_mid 保持 1500、装腿也按 1500µs 装 —— 装配基准由我们人为定义，
 * 不必追求舵机的物理 180° 中点（500~2900µs 时 1500µs 只对应物理 150°）。
 *
 * ⚠ NVS 只存 trim/dir，不存 us_span。改这里重烧后数值不丢，但同一个 trim
 * 产生的脉宽偏移按 span 比例缩小，站姿会偏移；换算关系：
 *   trim_new = trim_old × 旧span ÷ 新span = trim_old × 1.6667
 * 详见 docs/13-M1首次试走检查清单.md §3。 */
#define SERVO_US_MID_DEFAULT    1500
#define SERVO_US_SPAN_DEFAULT   1200

/* 裸脉宽（pwm / pwmscan / servo_raw_us）允许的电气测试范围。
 * 上限放宽到 3000µs 是为了测 360° 大行程位置舵机的真实端点（普通 180°
 * 舵机用不到这么高）。只作用于 raw 测试通道；正常角度映射仍由
 * us_mid / us_span 与电子硬限位 0~180° 约束。
 * 50Hz 下 3000µs = 15% 占空比；PCA9685 12bit 计数 614 < 4095，安全。 */
#define SERVO_US_CMD_MIN        500
#define SERVO_US_CMD_MAX        3000
#define SERVO_US_RAW_MIN        300
#define SERVO_US_RAW_MAX        3000

/* trim 允许范围（等效内部角，°）。
 * 原本 ±30° 只够补「舵机臂插歪几度」；本项目 8 只 360° 舵机的装配基准
 * 系统性偏移（髋约 −25°、膝约 +18°），2026-11-11 把 us_span 从 2000 改成 1200 后，
 * 为了让站姿脉宽保持不变，trim 需按 ×1.6667 换算，髋会到 −38~−43°。
 * 故放宽到 ±50°：这只是软件夹紧量，脉宽仍在 900~2100µs 内、sv 仍在 0~180° 内，
 * 不改动任何动作幅度。注意大 trim 会吃掉一侧硬限位余量，若 clampN 增长要调小步幅。 */
#define SERVO_TRIM_LIMIT_DEG    50.0f

/* 电子硬限位：任何情况下不输出超出这个范围的脉宽，防止顶死舵机 */
#define SERVO_DEG_HARD_MIN      0.0f
#define SERVO_DEG_HARD_MAX      180.0f

/* 单周期最大角度变化：20ms 内最多走 12°（=600°/s，MG90S 极限）。
 * 这是一道保命闸，正常步态最大只要 9.1°/tick，不会碰到它。 */
#define SERVO_MAX_STEP_DEG      12.0f

/* 动作/位姿切换时的角速度上限 °/s（比舵机极限略低，过渡更柔）。
 * 只用于 WALK / 模式切换时的兜底斜率限制；POSE 过渡走下面的专用缓动。 */
#define MOTION_SLEW_DPS         550.0f

/* ====================== 姿态过渡（蹲/趴/握手/挥手） ======================
 * 【2026-11-11 实测修 bug】旧实现把 8 个关节角各自按 11°/拍 等速逼近目标，
 * 但站→趴的行程是「髋 +28.5° / 膝 −57°」= 1:2。等速会让髋 3 拍就到位停住、
 * 膝还要再走 3 拍，中途破坏「髋 = −膝/2」，四只脚在地面上前滑 18mm 再滑回来，
 * 肉眼就是「大腿猛地下去、发抖、小腿随后才动、整体不自然」。
 * 现在改为：只用「膝角」一个标量做 smoothstep 缓动，髋角每拍由 IK 反解，
 * 脚永远在髋点正下方（前后位移恒 0），机身纯垂直升降，髋膝天然同步。
 * 900ms 时峰值膝角速度约 95°/s（髋 47.5°/s），远低于 600°/s 舵机极限。
 * 可用串口 `posedur <200-3000>` 现场调手感，不用重烧。 */
#define POSE_TRANS_MS_DEFAULT   900
#define POSE_TRANS_MS_MIN       200
#define POSE_TRANS_MS_MAX       3000

/* ====================== 动作切换：中间态站姿 ======================
 * 【2026-11-13 实测修 bug】旧版 motion_walk() 直接把相位清零、模式切成 WALK，
 * 下一拍立刻输出「站姿膝角 + 满步幅」。从蹲(-70°)/趴(-95°) 起步时目标角
 * 与当前角一拍内差 22~47°，限速闸只能让髋、膝各自按 11°/拍 往回赶，
 * 破坏「髋 = −膝/2」的 1:2 几何，四脚在地面打滑 → 肉眼就是「乱套」；
 * trot→walk 则是两套相位表硬切，等于四脚同时瞬移 → 顿挫。
 *
 * 现在所有切换都走同一个「道岔」：先把步幅/抬脚淡出到 0（此时 gait_step
 * 输出恒等于站姿几何），或把蹲/趴的膝角缓动收回站姿；站稳后再执行挂起的
 * 目标动作，并从 0 步幅平滑淡入。切换瞬间姿势连续，所以不抖不滑。
 *
 * fade 只缩放 speed_pct / turn_pct，不缩放 stride_mm（它同时是抬脚缩放的
 * 分母，改成 0 会除零）。fade=0 时 gait_step 四脚 y=0/lift=0 = 纯站姿。
 * 可用串口 `fadedur <200-2000>` 现场调手感，不用重烧。 */
#define GAIT_FADE_MS_DEFAULT    500
#define GAIT_FADE_MS_MIN        200
#define GAIT_FADE_MS_MAX        2000

/* ====================== 站姿停留（等真正站稳） ======================
 * 【2026-11-13 二次实测修 bug】上一版「回站姿」的计时一到就立刻执行挂起的
 * 下一个动作，实测仍会「乱套」。计时结束只代表【指令曲线走完】，不代表
 * 【舵机转到位、机身晃完】，中间差着两件事：
 *   ① s_out 经过 rate_limit 每拍最多只能走 11°，淡出/膝角回位途中可能
 *      还落后目标几度；下一拍目标就换成新动作，两者叠加又破坏髋膝 1:2。
 *   ② MG90S 是 0.1s/60°，PWM 指令到位后机械还要几十毫秒才停稳，再加齿隙
 *      回弹与四脚着地后的机身晃动，肉眼就是「还没站稳就又动了」。
 *
 * 现在 TO_STAND 结束后插入 TRANS_SETTLE：整段停留期间输出恒定站姿，
 * 必须同时满足【停留够时间】+【s_out 与站姿几何偏差 < 0.5° 且连续 3 拍】
 * 才放行挂起动作。另有 guard 超时兜底，避免永远收敛不了时卡死不动。
 * 可用串口 `standhold <100-2000>` 现场调，不用重烧。 */
#define GAIT_SETTLE_MS_DEFAULT   300
#define GAIT_SETTLE_MS_MIN       100
#define GAIT_SETTLE_MS_MAX       2000
#define GAIT_SETTLE_TOL_DEG      0.5f   /* 8 路关节角与站姿几何的最大允许偏差 */
#define GAIT_SETTLE_OK_BEATS     3      /* 连续收敛拍数（20ms/拍 = 60ms） */
#define GAIT_SETTLE_GUARD_MS     800    /* 收敛不了时叠加在停留时长上的强制放行上限 */

/* 差速转向力度：左右腿步幅 = 前进步幅 ± stride × turn_k × 这个系数。
 * 与 speed_pct 解耦，speed=0 时也能原地打转（旧实现整条 stride 被 speed
 * 乘掉，原地转向完全失效）。speed=100 时新公式与旧公式逐位相同。 */
#define GAIT_TURN_GAIN         0.6f
#define GAIT_SPIN_TURN_PCT     70      /* left/right 命令的默认转向力度 */

/* MG90S 标称速度：0.1s / 60° → 600°/s。
 * 超过这个值的指令角会"跑在机械角前面"，脚就绊了。
 * status 命令会拿它和当前参数算出的峰值角速度对比。 */
#define SERVO_MAX_RATE_DPS      600.0f

#endif /* SPIDER8_CONFIG_H */
