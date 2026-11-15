# Spider8 — 四足机器狗（ESP32-S3 蓝牙/网页遥控）

一个用 ESP32-S3 驱动的四足机器狗项目：8 路 360° 舵机（每腿髋+膝 2 关节）、
超声波避障、PCA9685 舵机驱动、BLE 手机遥控 + Wi-Fi 网页控制台。本仓库是
**M0（点亮）~ M3（蓝牙遥控）** 阶段的成果快照，步态机动与界面控制已基本可用。

## 硬件

- 主控：**ESP32-S3-N16R8**（内置 8 MB PSRAM / 16 MB Flash）
- 执行：8 × MG90S 360° 位置型舵机，双 PCA9685（I²C `0x40`，SDA=8 / SCL=9）
- 结构：乐高 Technic 拼装，每腿 2 关节（竖直面内折），靠左右步幅差动转向
- 避障：超声波传感器
- 供电：外部 5V 给舵机 V+，ESP32 由 3V3 供电，三方共地（**禁止只改 VCC 为 5V 而 SDA/SCL 直连 ESP32**）

## 目录结构

```
spider8/
├── firmware/            ESP-IDF 固件（ESP-IDF 5.5.5）
│   └── main/
│       ├── config.h     舵机/关节/姿态参数
│       ├── servo.*      舵机标定（trim/dir）、脉宽换算
│       ├── pca9685.*    I²C 舵机驱动
│       ├── gait.*       步态生成（波步/小跑/转向）
│       ├── motion.*     动作库与姿态过渡
│       ├── ble_ctl.*    NimBLE GATT 蓝牙遥控（服务 Spider8）
│       ├── wireless.*   Wi-Fi STA+SoftAP / 网页控制台 / OTA
│       ├── console.*    命令解析（与网页、BLE、串口共用）
│       └── web/         烧进固件的 console.html
├── docs/               各阶段文档（01~15）
├── tools/              Python 调试脚本（串口、BLE 探针、日志抓取等）
└── android/            安卓遥控 App（Kotlin，对应 servo_calib.html 的标定页）
```

## 固件构建与烧录

需要 ESP-IDF 5.5.5 环境。本机可用 `tools/idf.ps1` 作为统一入口：

```powershell
# 构建
tools/idf.ps1 -C firmware build
# 烧录（插标着 USB 的原生口，VID_303A；COM 号以 pyserial 能开为准）
tools/idf.ps1 -Port COMx flash
# 监视串口
tools/idf.ps1 -Port COMx monitor
```

**Wi-Fi OTA 救星**（USB 掉线时）：板子联网后

```bash
curl -X POST --data-binary @firmware/build/spider8.bin \
     -H "Expect:" -H "Content-Type: application/octet-stream" \
     http://<板子IP>/api/update
```

> 注意：OTA 会写空闲槽并自动重启；如需双槽同版，上传两次，或
> 用 esptool 同时写入 `0x20000` 与 `0x320000` 并清理 `otadata`。

## 安卓 App 构建

`android/` 为 Gradle 工程（AGP 8.7.0 / Kotlin 1.9.24，compileSdk 35），已内置
Gradle Wrapper（Gradle 8.9），**无需自行安装 Gradle**：

```bash
cd android
./gradlew assembleDebug        # Linux / macOS
gradlew.bat assembleDebug      # Windows
# 产物：app/build/outputs/apk/debug/app-debug.apk
```

也可用 Android Studio 打开，或本机 Gradle 命令行构建。App 功能：急停、6 个动作、
7 个行走/转向命令、4 个快捷命令、6 个参数滑块，以及对应 `servo_calib.html`
的舵机 trim 标定页（需先连上板子蓝牙）。

## 控制方式

- **蓝牙（推荐）**：手机连广播名 `Spider8`，使用 GATT 服务读写命令，命令与
  串口控制台一致（如 `walk 50`、`left`、`stand`、`status`、`trimget 0`）。
- **Wi-Fi 网页**：板子连路由后访问 `http://<板子IP>/`，内置 `console.html`
  控制台；`/api/cmd?c=` 可远程执行命令。

## 当前里程碑状态

- M0-1/M0-2 烧写与启动验收通过
- M1 舵机校准、M2 站立、M3 蓝牙遥控打通
- 步态（波步/小跑/后退/原地转向）与动作库（握手/挥手/演示）可用
- 安卓 App 主遥控页 + trim 标定页完成
- 已知遗留：超声波避障（M4）尚未实现；长时间 BLE 连接参数已加固但需真机复测

## 说明

舵机标定值存于板子 NVS，烧录不擦 NVS；源码中不含家庭 Wi-Fi 密码
（STA 密码仅运行时传入，存于设备 NVS）。`.workbuddy/` 为本地内部工作记忆，
不纳入本仓库。
