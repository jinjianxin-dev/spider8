# Spider8 固件（ESP32-S3-N16R8）

> 里程碑进度：**M0-1 工程骨架** ← 当前 ｜ M0-2 舵机驱动（下一步）

## 构建 / 烧写 / 监视

在 VS Code 中打开仓库根目录后：

| 操作 | 方式 |
|---|---|
| 编译 | `Ctrl+Shift+B`（任务 `ESP-IDF: 编译 (build)`） |
| 烧写 | `Ctrl+Shift+P → Run Task → ESP-IDF: 烧写 (flash)`，输入串口号如 `COM3` |
| 监视 | `Run Task → ESP-IDF: 串口监视 (monitor)`，波特率 115200 |
| 一键 | `Run Task → ESP-IDF: 编译+烧写+监视 (一键)` |

命令行等价（在 `firmware` 目录下）：

```powershell
..\tools\idf.ps1 build
..\tools\idf.ps1 -Port COM3 flash monitor
```

## M0-1 验收清单

烧写成功后，串口（115200）应看到：

- [ ] 横幅：`固件: spider8 v0.1.0`，ESP-IDF v5.5.5
- [ ] `芯片: ESP32-S3 rev?, 2 核`
- [ ] `Flash: 16 MB`（若只有 8MB → 开发板可能是 N8R8，请告知）
- [ ] `PSRAM: 8192 KB`（若是 0 → 告警提示检查 Octal 配置）
- [ ] 分区表列出：`otadata / ota_0(3072KB) / ota_1(3072KB) / littlefs / coredump`
- [ ] 1Hz 心跳日志持续输出，`free heap` 数值稳定（无泄漏迹象）
- [ ] GPIO4 接 LED 时 1Hz 闪烁（可选验证）

## 目录结构

```
firmware/
├── CMakeLists.txt        # 工程入口（项目名/版本：spider8 v0.1.0）
├── partitions.csv        # 16MB 分区：双OTA + littlefs(动作库) + coredump
├── sdkconfig.defaults    # 默认配置：S3 240MHz / 16MB QIO / Octal PSRAM / 1kHz tick
└── main/
    ├── CMakeLists.txt
    └── main.c            # M0-1：系统信息横幅 + 心跳任务（静态分配栈）
```

## 后续里程碑

- M0-2：舵机驱动层（LEDC 8 通道 + 软限位 + 中位 trim，GPIO 4/5/6/7/15/16/17/18）
- M0-3：模式仲裁框架（事件组）
