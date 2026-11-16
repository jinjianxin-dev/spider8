# Spider8 — 四足机器狗（ESP32-S3 语音 / 蓝牙遥控）

一个用 ESP32-S3 驱动的四足机器狗：8 路 360° 舵机（每腿髋+膝 2 关节）、
PCA9685 舵机驱动，支持**离线语音控制**（唤醒词「你好小智」+ 25 条固定命令词）、
BLE 手机遥控与 Wi-Fi 网页控制台。

> 当前跑的是 **I2 语音固件**：语音识别经安全门 → 统一命令入口 `cli_exec()` → 步态层。
> 语音联调期 `CONFIG_SPIDER8_WIFI_ENABLED=n`（**Wi-Fi 与网页控制台当前关闭**），
> BLE 与语音同板共存。完整方案与验收记录见 `docs/16-语音控制机器狗-集成方案.md`。

## 硬件

- 主控：**ESP32-S3-N16R8**（8 MB PSRAM / 16 MB Flash）
- 执行：8 × MG90S 360° 位置型舵机 + PCA9685（I²C `0x40`，SDA=8 / SCL=9）
- 结构：乐高 Technic，每腿 2 关节；2-DOF 不能横移，转向靠左右步幅差动
- 语音输入：INMP441 麦克风（I2S0：`SCK→5 / WS→6 / SD→7 / VDD→3V3`）
- 语音输出：MAX98357 + 喇叭（I2S1：`BCLK→15 / LRC→16 / DIN→18`）
  - ⚠️ `VIN` **不要接 ESP32 板的 `5V` 脚**（该脚防倒灌，USB 供电时实测仅 0.37 V），
    要接**舵机 5V / PCA9685 的 `V+`**；`GND` 三方共地，`SD` 悬空（接 GND = 关断），
    喇叭两根线分别接 `OUTP` / `OUTN`，任一根都别接地。
- 供电：外部 5V 给舵机 `V+`（**充电宝必须开开关且有带载能力**），
  ESP32 由 3V3 供电；**禁止只把 PCA9685 的 VCC 改 5V 而 SDA/SCL 直连 ESP32**
- 预留未实现：超声波避障（M4，词表 38/39 已留位）、IMU 跌倒自恢复

## 目录结构

```
spider8/
├── firmware/            ESP-IDF 5.5.5 固件
│   └── main/
│       ├── config.h / servo.* / pca9685.*   舵机标定与驱动
│       ├── gait.* / motion.*                步态生成、动作库与姿态过渡
│       ├── console.* / cli_out.*            统一命令入口（串口/BLE/网页/语音共用）
│       ├── ble_ctl.*                        NimBLE GATT（广播名 Spider8）
│       ├── wireless.*                       Wi-Fi STA+SoftAP / 网页控制台 / OTA（当前关闭）
│       ├── voice.* / mouth.* / voice_clips.*   esp-sr 语音前端 + 喇叭播报
│       └── web/console.html                 烧进固件的网页控制台
├── android/             Kotlin 遥控 App（急停 / 语音安全开关 / 动作 / 行走 / trim 标定）
├── docs/                01~15 阶段文档 + 16 语音集成方案（v2.0）
└── tools/               串口、BLE 探针、语音口测与验收脚本
```

## 固件构建与烧录

统一入口 `tools/idf.ps1`（调前补 `$env:PATHEXT` 与 `$env:LOCALAPPDATA`）：

```powershell
tools/idf.ps1 -C firmware build
tools/idf.ps1 -Port COMx flash / monitor
```

⚠️ **Wi-Fi 关着就没有 OTA，只能 USB 全量刷**。必须插标着 **`USB`** 的原生口
（`VID_303A`，CH340 那个口驱动是坏的）；COM 号不固定，以 pyserial 能打开为准。
标准流程：esptool 写 bootloader(0x0) / 分区表(0x8000) / otadata(0x10000) /
**两个 OTA 槽写同一版**(0x20000 + 0x320000) / 语音模型(0x800000)，
`--after no_reset` 后**再跑一次** `chip_id --before no_reset --after hard_reset` 才真启动。
**烧录不擦 NVS**，8 路舵机 trim 保留。

## 安卓 App 构建

`android/` 为 Gradle 工程（AGP 8.7.0 / Kotlin 1.9.24，compileSdk 35）。
README 保留 Wrapper 便于 clone 后开箱即建；本机离线构建可直接用已解压的 Gradle 8.9：

```powershell
$env:JAVA_HOME = 'C:\Users\aaron\.jdks\jbr-17.0.14'
$env:GRADLE_USER_HOME = 'E:\gradle_home'
& 'C:\Users\aaron\.gradle\wrapper\dists\gradle-8.9-bin\90cnw93cvbtalezasaz0blq0a\gradle-8.9\bin\gradle.bat' assembleDebug --offline
# 产物：android/app/build/outputs/apk/debug/app-debug.apk
```

App 功能：急停、**语音动作 开/关（安全开关）**、6 个姿态、7 个行走/转向命令、
4 个参数滑块、命令终端，以及舵机 trim 标定页。
⚠️ 「后退（未开放）」是禁用按钮：现有步态层把负速度夹成继续前进，按了会往前走。

## 控制方式

| 通道 | 说明 |
|---|---|
| **语音（默认）** | 说「你好小智」→ 等「我在」播完 + 半秒 → 说命令词（8 秒窗口）。25 条固定词，含「停」急停直通、5 秒行走自动站定 |
| **蓝牙** | 手机连广播名 `Spider8`，GATT 写命令；命令与串口完全一致（`walk 50`、`pose stand`、`status`、`voice`、`trimget 0`…） |
| Wi-Fi 网页 | **当前固件已关闭**；开启需 `CONFIG_SPIDER8_WIFI_ENABLED=y` 重编，且要重测片内 SRAM（esp-sr 的 AFE 吃片内内存） |
| 串口 | USB Serial/JTAG，115200，命令同上 |

上电默认 **`exec=1`（语音放行）/ `mute=0`（播报）**，重启自动恢复。
⚠️ **把狗放到地上、搬动或收纳前，先 `voice exec off` 或用 App 的语音开关关掉**；
这个关闸是临时的，下次重启又会放行。

## 当前里程碑状态

- ✅ M0 点亮 / M1 舵机校准 / M2 站立 / M3 BLE 遥控 + 安卓 App
- ✅ 语音 I0 内存门禁、I1 合壳上板、I2 语音驱动动作（含真语音行走验收）
- ✅ 喇叭出声、有播报条件下复验、上电默认全开（重启免手动开闸）
- ✅ 安卓 App 语音安全开关、后退误动作禁用、BLE 回话串台修复
- ⏳ 逐动作验收 / 50 条口测 ≥90% / 表外词 0 执行 / BLE 与语音三通道仲裁
- ⏳ 后退能力（需改步态层）、`demo` 与 5 秒超时冲突、M4 超声波避障
- ⏳ 电池电压监测（「日志全绿但腿不动」多半是充电宝断电）

## 说明

舵机标定值存于板子 NVS，烧录不擦 NVS；源码中不含家庭 Wi-Fi 密码。
`.workbuddy/` 为本地内部工作记忆，不纳入本仓库。
