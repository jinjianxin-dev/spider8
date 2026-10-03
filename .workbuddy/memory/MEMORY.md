# Spider8 项目长期记忆

## 项目定位
- 四足蜘蛛机器人：ESP32-S3-N16R8 主控，8×MG90S 舵机（每腿 2 关节），BLE 微信小程序控制，超声波避障，乐高 Technic 结构先行。
- 推进方式：严格按「一步一个小功能」迭代；每步交付可验证产物；先文档（需求→设计）后编码。

## 关键路径与环境约定
- 工程根目录：`C:\Users\aaron\WorkBuddy\spider8`
- ESP-IDF 安装位置：`E:\esp\esp-idf`（v5.5.5，子模块已全部修复），工具链 `E:\esp\.espressif`
- ESP-IDF 开发用 Python：必须用 venv `E:\esp\.espressif\python_env\idf5.5_py3.13_env`（PATH 里系统 python 3.14.6 不可用）
- **本机 git 全局配置有失效的 ghproxy.com 代理改写**（url.*.insteadof）→ 任何 github 克隆必须用 `GIT_CONFIG_GLOBAL=/dev/null` 绕过
- IDF 构建链路已验证：`tools/idf.ps1 build` 可直接编译（id.ps1 自动激活 venv）
- 文档编号规则：`docs/0N-<主题>.md`（01 需求、02 设计、03 环境、04 VSCode、…）
- 里程碑：M0-1 固件骨架已完成并编译通过（spider8.bin 202KB）；M0-2 舵机驱动（LEDC GPIO 4/5/6/7/15/16/17/18）

## 代码/脚本规范
- **所有 .ps1 必须保存为 UTF-8 with BOM + CRLF**（PowerShell 5.1 否则中文乱码导致语法错误）
- 新增 .ps1 后必须做语法校验：`[Parser]::ParseFile(...)` 且实跑验证
- PowerShell 工具输出捕获可能为空 → 用「脚本写文件 + Read 读取」拿结果
- 禁止从 Bash 调用 powershell.exe（安全策略），改用 PowerShell 工具
- 固件遵循：FreeRTOS 静态分配、ISR 最小化、外设驱动必须处理错误路径、不阻塞等待

## 里程碑
- M0 固件骨架+舵机点动 → M1 校准+单腿 → M2 站立 → M3 遥控爬行 → M4 避障 → M5 动作库 → M6 外观联调
