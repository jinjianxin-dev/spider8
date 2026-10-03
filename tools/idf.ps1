<#
    Spider8 项目 —— idf.py 环境包装脚本
    作用：自动激活 ESP-IDF 环境（点源 export.ps1），然后转发参数给 idf.py。
    这样在 VS Code 任务、普通终端里都能直接跑，无需手动 export。

    用法示例：
        .\tools\idf.ps1 build                    编译
        .\tools\idf.ps1 -Port COM7 flash         烧写
        .\tools\idf.ps1 -Port COM7 monitor       串口监视
        .\tools\idf.ps1 -Port COM7 flash monitor 烧写+监视
        .\tools\idf.ps1 set-target esp32s3       设置目标芯片
        .\tools\idf.ps1 menuconfig               图形化配置

    注意：需在 ESP-IDF 工程目录下执行（含 CMakeLists.txt 的目录）。
#>
[CmdletBinding()]
param(
    # 串口号，例如 COM7（不填则用 idf.py/esptool 自动探测）
    [string]$Port = "",

    # 透传给 idf.py 的其余参数
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------- 定位 ESP-IDF
$IdfPath   = if ($env:IDF_PATH)       { $env:IDF_PATH }       else { "E:\esp\esp-idf" }
$ToolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "E:\esp\.espressif" }
$ExportScript = Join-Path $IdfPath "export.ps1"

if (-not (Test-Path $ExportScript)) {
    Write-Host ""
    Write-Host "[X] 未找到 ESP-IDF 激活脚本：" -ForegroundColor Red
    Write-Host "    $ExportScript" -ForegroundColor Red
    Write-Host ""
    Write-Host "    请先完成环境安装：" -ForegroundColor Yellow
    Write-Host "      1) cd C:\Users\aaron\WorkBuddy\spider8" -ForegroundColor Gray
    Write-Host "      2) Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass" -ForegroundColor Gray
    Write-Host "      3) .\tools\install_esp_idf.ps1" -ForegroundColor Gray
    Write-Host ""
    exit 1
}

# 若环境变量指向的位置没有 python_env（外部残留的错路径），回退到默认安装位置
if (-not (Test-Path (Join-Path $ToolsPath "python_env"))) {
    foreach ($alt in @("E:\esp\.espressif", (Join-Path $env:USERPROFILE ".espressif"))) {
        if (Test-Path (Join-Path $alt "python_env")) { $ToolsPath = $alt; break }
    }
}

$env:IDF_PATH       = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath

# ---------------------------------------------------------------- 硬绑定 IDF 自带的 python venv
# 为什么必须这么做：ESP-IDF 的 activate.py 会用「当前 python 的版本号」去拼虚拟环境目录名
# （如 idf5.5_py3.13_env）；若 PATH 里首选的是系统 python 3.14，就会拼成 idf5.5_py3.14_env
# 而报 "virtual environment not found"。
# 所以这里同时做两件事：① 直接指定 IDF_PYTHON_ENV_PATH（idf_tools 优先采信）；② 把 venv 的
# python 提到 PATH 最前，保证后续 check-python-dependencies / idf.py 都跑在 venv 里。
$envRoot    = Join-Path $ToolsPath "python_env"
$venvDir    = $null
$venvPython = $null

if (Test-Path $envRoot) {
    $candidates = Get-ChildItem -Path $envRoot -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -like "idf*_env" } |
                  Sort-Object Name -Descending
    foreach ($d in $candidates) {
        $p = Join-Path $d.FullName "Scripts\python.exe"
        if (Test-Path $p) { $venvDir = $d.FullName; $venvPython = $p; break }
    }
}

if ($null -eq $venvPython) {
    Write-Host ""
    Write-Host "[X] 未找到 ESP-IDF 的 python 虚拟环境：$envRoot" -ForegroundColor Red
    Write-Host ""
    Write-Host "    这通常说明 ESP-IDF 没装完整，请重新运行安装脚本：" -ForegroundColor Yellow
    Write-Host "      .\tools\install_esp_idf.ps1" -ForegroundColor Gray
    Write-Host ""
    exit 1
}

$env:IDF_PYTHON_ENV_PATH = $venvDir
$env:PATH = (Split-Path $venvPython) + ";" + $env:PATH
Write-Host "    IDF  : $IdfPath" -ForegroundColor DarkGray
Write-Host "    venv : $venvDir" -ForegroundColor DarkGray

# 点源激活（必须用 . 前缀，否则 export.ps1 定义的 idf.py 函数不会留在当前作用域）
# PS 5.1 会把子进程 stderr 输出误判为终止错误，故此处不能设 ErrorActionPreference=Stop
. $ExportScript | Out-Null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "[X] ESP-IDF 环境激活失败（未生成 idf.py 入口）" -ForegroundColor Red
    Write-Host "    排查建议：直接运行 .\tools\check_env.ps1 看哪一项是 FAIL" -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

# ---------------------------------------------------------------- 组装参数
$finalArgs = @()
if ($Port -ne "") { $finalArgs += @("-p", $Port) }
if ($null -ne $IdfArgs -and $IdfArgs.Count -gt 0) { $finalArgs += $IdfArgs }
if ($finalArgs.Count -eq 0) { $finalArgs = @("--help") }

Write-Host ""
Write-Host ">>> idf.py $($finalArgs -join ' ')" -ForegroundColor Cyan
Write-Host "    工程目录: $(Get-Location)" -ForegroundColor DarkGray
Write-Host ""

# ---------------------------------------------------------------- 编码准备
# 只在串口监视时切到 UTF-8：Windows 控制台默认代码页 936(GBK)，而固件日志是 UTF-8，
# 不切换中文会显示成乱码。menuconfig 是全屏 TUI，保持默认代码页更稳，故不切换。
if ($finalArgs -contains "monitor") {
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        chcp 65001 | Out-Null
    } catch { }
}

# ---------------------------------------------------------------- 执行
idf.py @finalArgs

exit $LASTEXITCODE
