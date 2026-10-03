# Step 1 补充：在 VS Code 中开发（两条路线）

> 日期：2026-10-02 ｜ 结论：**完全可以，VS Code 是 ESP-IDF 官方主推的 IDE**
> 前置：先完成 `docs/03-环境搭建.md` 的 ESP-IDF 安装（装完 `E:\esp\esp-idf` 存在）

---

## 0. 一句话结论

VS Code 用起来有两种方式，**不冲突，可随时切换**：

| 路线 | 依赖 | 能力 | 推荐场景 |
|---|---|---|---|
| **A. 零扩展（已为你配好）** | 只用 VS Code 自带功能 | 编译/烧写/监视/菜单配置 一键按钮 + 终端 + 语法补全 | 立刻可用、不怕扩展版本变动 |
| **B. 官方扩展（Espressif IDF）** | 需装扩展 | 图形化状态栏、串口选择、断点调试、芯片概览 | 想要可视化调试时再加装 |

> 两者本质都是调用 `idf.py`，**路线 A 永远有效**，所以我先把它配好。

---

## 1. 路线 A：零扩展（已配置完成，推荐先用）

### 1.1 已生成的文件

```
spider8/
├── .vscode/
│   ├── tasks.json              # 7 个一键任务（每个任务自带 options.shell，不受默认终端影响）
│   ├── settings.json           # 默认终端=ESP-IDF Shell；隐藏 build 目录
│   └── c_cpp_properties.json   # 语法补全指向 compile_commands.json
└── tools/
    ├── idf.ps1                 # 【任务用】环境包装器：激活 IDF 后转发给 idf.py
    └── idf_shell.ps1           # 【终端用】激活后保持会话，供 ESP-IDF Shell 终端启动
```

### 1.2 使用方式

1. VS Code → **文件 → 打开文件夹** → 选择 `C:\Users\aaron\WorkBuddy\spider8`
2. 按 **`Ctrl + Shift + B`** → 编译（`idf.py build`）
3. 按 **`Ctrl + Shift + P`** → 输入 `Run Task` → 选择：
   - `ESP-IDF: 烧写 (flash)`（会弹窗问串口号，**默认已填 `COM7`**，回车即可）
   - `ESP-IDF: 串口监视 (monitor)`
   - `ESP-IDF: 编译+烧写+监视 (一键)`
   - `ESP-IDF: 设置目标芯片 (esp32s3)`
   - `ESP-IDF: 菜单配置 (menuconfig)`
   - `ESP-IDF: 全量清理 (fullclean)`
4. 内置终端默认就是 **"ESP-IDF Shell"**（启动即激活环境，可直接敲 `idf.py build`、`idf.py -p COM7 flash monitor`）
   - 若想用干净的原生 PowerShell：终端面板右上角 `+` 旁边的下拉箭头 → 选 **PowerShell**
   - 注意：ESP-IDF Shell 带 `-NoExit`（激活后保持会话），所以**别用它来跑任务**（见 §4.2，任务已自带独立 shell，不受影响）

> 串口号修改：`tasks.json` 顶部 `inputs` 里的 `"default"` 改成你的实际串口；运行时弹窗输入的值只在本次生效。

### 1.3 代码补全（IntelliSense）

1. 装 VS Code 扩展 **C/C++**（`ms-vscode.cpptools`，微软官方，免费）
2. 首次编译后 ESP-IDF 会生成 `firmware/build/compile_commands.json`
3. `c_cpp_properties.json` 已指向它 → `#include "driver/ledc.h"` 等会有补全与跳转

### 1.4 说明

任务的工作目录是 `${workspaceFolder}/firmware`，**该目录在 M0-1 建立固件工程后才会出现**。在固件工程创建前点这些任务会提示目录不存在，属正常现象。

---

## 2. 路线 B：官方扩展 Espressif IDF

### 2.1 安装与配置

1. 扩展市场搜索 **Espressif IDF**（`espressif.esp-idf-extension`）→ 安装
2. `Ctrl + Shift + P` → **ESP-IDF: Configure ESP-IDF Extension**
3. 选择 **Use existing setup / EXISTING SETUP**（不要选 Install，否则会再下一份 2GB 的 IDF）
4. 三个路径按下表填写：

| 配置项 | 填写值 |
|---|---|
| ESP-IDF 目录 | `E:\esp\esp-idf` |
| Tools 目录 | `E:\esp\.espressif` |
| Python | `C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12\python.exe` |

> ⚠️ **Python 千万别选 PATH 默认的 3.14.6**，超出 IDF 5.5 支持范围（3.9~3.13），会导致工具报错。

5. 配置完成后底部状态栏出现：目标芯片选择 / 🔨 编译 / 🔥 烧写 / 📺 监视 / 🐞 调试

### 2.2 版本差异提醒

最新版扩展改用 **EIM（ESP-IDF Installation Manager）配置模型**（配置项为 `idf.currentSetup` / `idf.eimIdfJsonPath`），
如果你的扩展向导里**找不到 "Use existing setup"**，说明它要求用 EIM 重新安装 IDF —— 这时：

- **推荐**：直接用路线 A（功能完全够用），或
- 在扩展里改用 **较稳定的 1.9.x 版本**（扩展页 → Install Another Version…）再配置现有安装

### 2.3 断点调试（加分项）

ESP32-S3 自带 **USB-Serial/JTAG**，只需一根 USB 线即可硬件断点调试（无需额外调试器）：
- 扩展 → `Ctrl+Shift+P` → **ESP-IDF: OpenOCD Manager** 启动 → 侧栏 Run and Debug → 选 ESP-IDF 配置
- 注意：调试会占用 USB 口，此时串口监视需改用第二个 UART 或虚拟串口

---

## 3. 推荐工作流（我们俩配合）

```
我（AI）：写 / 改固件代码，保证逻辑与结构正确
   ↓
你（VS Code）：Ctrl+Shift+B 编译 → Run Task 烧写 → 监视窗口看日志
   ↓
把日志 / 现象贴回给我 → 我定位问题 → 下一小步
```

烧写和接线必须由你操作（我碰不到硬件）；编译可以在你机器上随时验证。

---

## 4. 排错表

| 现象 | 原因 | 解决 |
|---|---|---|
| **任务一直显示"正在执行"、永不结束** | 默认终端 profile（ESP-IDF Shell）带 `-NoExit`，且 profile 的 `-Command` 会把任务命令行吞成它自己的参数，任务是空转的 | `tasks.json` 每个任务已用 `options.shell` 显式指定干净 shell（见 §4.2）；对已经卡住的任务，点终端标签上的 **🗑 结束终端** 图标即可 |
| **`Python virtual environment "...idf5.5_py3.14_env\Scripts\python.exe" not found`** | PATH 首选的 `python` 是 3.14，而 IDF 的 `activate.py` 会用「当前 python 的版本号」去拼 venv 目录名（`idf5.5_py3.13_env` 才对） | 终端启动脚本 `tools/idf_shell.ps1` 与任务包装器 `tools/idf.ps1` 都已**硬绑定 `IDF_PYTHON_ENV_PATH`**；若仍报错，确认 `E:\esp\.espressif\python_env\idf5.5_py3.13_env` 存在 |
| 同一个错误跟着 `. $idf_exports` / `表达式生成无效的对象` | 上一条的连带后果：`activate.py` 失败 → `$idf_exports` 为空 → 点源空字符串报错 | 同上，根因是 venv 没找到，不是 PowerShell 语法问题 |
| 监视窗口中文显示成 `??????` 或乱码 | Windows 控制台代码页 936(GBK)，而固件日志是 UTF-8 | 监视任务已自动切 `chcp 65001`；若仍乱码，把 VS Code 终端编码设为 UTF-8 |
| 终端提示 `idf.py 不是内部或外部命令` | 终端不是 ESP-IDF Shell | VS Code 右下角 `+` 号旁下拉选 **ESP-IDF Shell**；或直接用任务按钮 |
| 任务报 `cwd 不存在` | 还没建固件工程 | 等 M0-1 创建 `firmware/` 后再用 |
| 扩展报 `ESP-IDF Path invalid` | 路径填错 / 填了 tools 目录 | IDF 目录填 `E:\esp\esp-idf`（含 `export.ps1` 的那层） |
| 扩展里 Python 列表里只有 3.14 | 未手动指定 | 手动浏览选择 `...\3.13.12\python.exe` |
| `Monitor` 启动失败 `/dev/ttyUSB0 拒绝访问` | 串口被其它程序占用 | 关掉其它串口助手 / 关闭上一个 monitor 任务 |
| 烧写一直 `Connecting....` | 未进下载模式 / 串口选错 | 按住 **BOOT** 键 → 点 **RST** → 松开 BOOT；确认串口号 |
| 编译报错但代码看着没问题 | 增量缓存脏了 | Run Task → `ESP-IDF: 全量清理 (fullclean)` 后重新编译 |
| 打开 vs 关闭 VS Code 后补全失效 | 未编译，缺 compile_commands.json | 先编译一次 |

---

## 4.1 深挖：`py3.14_env not found` 的完整根因（2026-10-03 实修）

这是本项目踩过最隐蔽的坑，记录原理避免复发：

```
tools/idf.ps1
   └─ . E:\esp\esp-idf\export.ps1
          └─ $idf_exports = python "tools/activate.py" --export     ← 这里用的是 PATH 里的 python
                 └─ activate.py
                        └─ idf_tools.get_python_env_path()
                               venv路径 = IDF_PYTHON_ENV_PATH ?? <ToolsPath>/python_env/idf<版本>_py<当前python主.次>_env
                                                                                        ↑ 关键：跟着"当前 python"走
```

本机 `PATH` 首选 python 是 **3.14.6**（`C:\Users\aaron\AppData\Local\Python\bin\python.exe`），
于是 IDF 去拼 `idf5.5_py3.14_env` —— 而实际只装了 `idf5.5_py3.13_env` → 找不到 → 报错。

**修复方式**（已写进 `tools/idf.ps1`，双保险）：

1. **硬绑定**：扫描 `E:\esp\.espressif\python_env\idf*_env`，把找到的目录直接赋给 `$env:IDF_PYTHON_ENV_PATH`
   （`idf_tools.py` 对该变量**最高优先级**，见 `get_python_env_path()`）
2. **PATH 前置**：把 venv 里的 `python.exe` 目录插到 `PATH` 最前，保证后续 `check-python-dependencies`、`idf.py` 都跑在 venv 内
3. **找不到就明确报错**，不再"静悄悄退化成系统 python"导致后面一堆玄学错误

> 排查同类问题的万能命令：
> ```powershell
> E:\esp\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe E:\esp\esp-idf\tools\activate.py --export
> ```
> 能直接跑通说明 venv 本身没问题，问题在"谁来调它"。

---

## 4.2 深挖：任务"永远在执行中"的根因（2026-10-03 实修）

### 现象

点 `ESP-IDF: 烧写 (flash)` 后，终端里报 venv 找不到，而且**任务一直卡在"正在执行"**，永不结束。

### 根因（两个 bug 叠在一起）

`settings.json` 里我把 **"ESP-IDF Shell" 设成了默认终端**，而它的 args 带 `-NoExit` 和一个 `-Command`：

```
powershell -NoExit -ExecutionPolicy Bypass -Command ". 'E:\esp\esp-idf\export.ps1'"
```

VS Code 跑 shell 任务时，若没有单独指定 shell，会**用默认 profile**，并把任务命令**追加到 profile 参数后面**。于是真实进程命令行变成：

```
powershell.exe -NoExit -ExecutionPolicy Bypass -Command . 'E:\esp\esp-idf\export.ps1'
        -Command powershell -NoProfile -ExecutionPolicy Bypass -File ...\idf.ps1 -Port COM7 flash
                                                     ↑ 这串被当成 export.ps1 的参数吃掉了
```

后果有两个，正好对应两个现象：

1. **报错**：真正执行的其实是 profile 里那句裸 `. export.ps1` —— 环境里没有 `IDF_TOOLS_PATH`、`python` 又是 3.14，于是它去找 `C:\Users\aaron\.espressif\python_env\idf5.5_py3.14_env` → `not found`。**跟 `idf.ps1` 无关。**
2. **不结束**：`-NoExit` 让这个 shell 永不退出 → VS Code 判定任务"仍在执行中"。

> 所以：**带 `-NoExit` 的终端 profile 不能用来跑任务**。

### 修复

1. **任务自带 shell**：`tasks.json` 里每个任务都加了 `options.shell`，显式指定干净 shell，
   并且 `command` 直接就是脚本路径，不再套一层 powershell：

   ```json
   "command": "${workspaceFolder}\\tools\\idf.ps1",
   "args": ["-Port", "${input:serialPort}", "flash"],
   "options": {
     "cwd": "${workspaceFolder}/firmware",
     "shell": { "executable": "powershell.exe",
                "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File"] }
   }
   ```

   → 实际执行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File <idf.ps1> -Port COM7 flash`，
   进程跑完即退出，**与默认终端 profile 完全解耦**。

2. **终端 profile 换成专用脚本**：`ESP-IDF Shell` 改为调用 `tools/idf_shell.ps1`（先钉死环境再激活），
   这样手动开终端也不会再报 `py3.14` 错。该脚本用 `return` 而不是 `exit`，避免把终端一起关掉。

3. **去掉"按键关闭终端"提示**：所有任务加 `"presentation": { "showReuseMessage": false }`。

### 已卡住的任务怎么清掉

任选其一：
- 点该终端标签右侧的 **🗑（结束终端 / Kill Terminal）** 图标
- 或 `Ctrl+Shift+P` → **Tasks: Terminate Task** → 选 `ESP-IDF: 烧写 (flash)`
- 或直接关掉那个终端面板（`Ctrl+Shift+W` 或点 `×`）

### 验证结果

| 验证项 | 命令 | 结果 |
|---|---|---|
| 终端 profile 路径 | `powershell -NoExit -Command ". 'tools\\idf_shell.ps1'; exit"` | `[OK] ESP-IDF 环境已就绪：ESP-IDF v5.5.5-dirty`，退出码 0 |
| 任务实际命令行 | `powershell -NoProfile -ExecutionPolicy Bypass -File tools\\idf.ps1 build`（cwd=firmware） | `Project build complete`，22.8 秒后**自行退出**（退出码 0） |

---

---

## 5. 本步验收标准

- [ ] 打开 `spider8` 文件夹，内置终端显示 `(esp-idf)` 前缀
- [ ] `Ctrl+Shift+B` 能触发编译任务（无固件工程时提示目录不存在也算链路通）
- [ ] `Ctrl+Shift+P → Run Task` 能看到 7 个 `ESP-IDF:` 任务
- [ ] C/C++ 扩展已装（补全在 M0-1 编译后生效）

> 完成后再进入 **M0-1：建立固件工程骨架 + 编译烧写 hello_world，打通电脑↔开发板链路**。
