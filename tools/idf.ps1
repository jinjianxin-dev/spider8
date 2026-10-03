<#
    Spider8 项目 —— idf.py 环境包装脚本
    作用：自动激活 ESP-IDF 环境（点源 export.ps1），然后转发参数给 idf.py。
    这样在 VS Code 任务、普通终端里都能直接跑，无需手动 export。

    用法示例：
        .\tools\idf.ps1 build                    编译
        .\tools\idf.ps1 -Port COM3 flash         烧写
        .\tools\idf.ps1 -Port COM3 monitor       串口监视
        .\tools\idf.ps1 -Port COM3 flash monitor 烧写+监视
        .\tools\idf.ps1 set-target esp32s3       设置目标芯片
        .\tools\idf.ps1 menuconfig               图形化配置

    注意：需在 ESP-IDF 工程目录下执行（含 CMakeLists.txt 的目录）。
#>
[CmdletBinding()]
param(
    # 串口号，例如 COM3（不填则用 idf.py/esptool 自动探测）
    [string]$Port = "",

    # 透传给 idf.py 的其余参数
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = "Stop"

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

# ---------------------------------------------------------------- 激活环境
# 必须用点源(.)，否则 export.ps1 里定义的 idf.py 函数不会留在当前作用域
$env:IDF_PATH       = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
. $ExportScript | Out-Null

# ---------------------------------------------------------------- 组装参数
$finalArgs = @()
if ($Port -ne "") { $finalArgs += @("-p", $Port) }
if ($null -ne $IdfArgs -and $IdfArgs.Count -gt 0) { $finalArgs += $IdfArgs }
if ($finalArgs.Count -eq 0) { $finalArgs = @("--help") }

Write-Host ""
Write-Host ">>> idf.py $($finalArgs -join ' ')" -ForegroundColor Cyan
Write-Host "    ESP-IDF: $IdfPath" -ForegroundColor DarkGray
Write-Host "    工程目录: $(Get-Location)" -ForegroundColor DarkGray
Write-Host ""

# ---------------------------------------------------------------- 执行
if (Get-Command idf.py -ErrorAction SilentlyContinue) {
    idf.py @finalArgs
} else {
    # 兜底：export.ps1 未定义 idf.py 函数时直接调 python 入口
    $idfPyEntry = Join-Path $IdfPath "tools\idf.py"
    & python $idfPyEntry @finalArgs
}

exit $LASTEXITCODE
