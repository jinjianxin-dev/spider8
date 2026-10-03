# Spider8 项目长期记忆

## 项目定位
- 四足蜘蛛机器人：ESP32-S3-N16R8 主控，8×MG90S 舵机（每腿 2 关节），BLE 微信小程序控制，超声波避障，乐高 Technic 结构先行。
- 推进方式：严格按「一步一个小功能」迭代；每步交付可验证产物；先文档（需求→设计）后编码。

## 关键路径与环境约定
- 工程根目录：`C:\Users\aaron\WorkBuddy\spider8`
- ESP-IDF 安装位置：`E:\esp\esp-idf`，工具链 `E:\esp\.espressif`（C/D 盘空间紧张，不要装 C 盘）
- ESP-IDF 版本：v5.5.5（Gitee 镜像克隆，因 github 慢）
- 开发用 Python：3.13.12（`C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12\`）；PATH 默认的 3.14.6 超出 IDF 支持范围
- 文档编号规则：`docs/0N-<主题>.md`（01 需求分析、02 设计方案、03 环境搭建 …）

## 代码/脚本规范
- **所有 .ps1 必须保存为 UTF-8 with BOM + CRLF**（PowerShell 5.1 否则中文乱码导致语法错误）
- 新增 .ps1 后必须做语法校验：`[Parser]::ParseFile(...)` 且实跑验证
- PowerShell 工具输出捕获可能为空 → 用「脚本写文件 + Read 读取」拿结果
- 禁止从 Bash 调用 powershell.exe（安全策略），改用 PowerShell 工具
- 固件遵循：FreeRTOS 静态分配、ISR 最小化、外设驱动必须处理错误路径、不阻塞等待

## 里程碑
- M0 固件骨架+舵机点动 → M1 校准+单腿 → M2 站立 → M3 遥控爬行 → M4 避障 → M5 动作库 → M6 外观联调
