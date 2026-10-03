<#
    Spider8 —— VS Code「ESP-IDF Shell」终端启动脚本

    由 .vscode/settings.json 的终端 profile 以「点源」方式调用（配合 -NoExit 保持会话）：
        powershell -NoExit -ExecutionPolicy Bypass -Command ". 'C:\Users\aaron\WorkBuddy\spider8\tools\idf_shell.ps1'"

    为什么需要它（而不是直接 . export.ps1）：
      export.ps1 会用【当前 python 的版本号】去拼 venv 目录名。裸环境里 PATH 首选 python 是
      系统 3.14、且没有 IDF_TOOLS_PATH，于是它去找 C:\Users\aaron\.espressif\python_env\
      idf5.5_py3.14_env 而报 "virtual environment not found"（详见 docs/04 §4.1）。
      这里先按 tools/idf.ps1 完全相同的规则把环境钉死，再激活，就不会踩坑。

    注意：本脚本用「点源」方式被调用，所以不能写 exit —— 否则会把整个终端会话关掉。
#>
$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------- 定位 ESP-IDF
$IdfPath   = if ($env:IDF_PATH)       { $env:IDF_PATH }       else { "E:\esp\esp-idf" }
$ToolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "E:\esp\.espressif" }

if (-not (Test-Path (Join-Path $ToolsPath "python_env"))) {
    foreach ($alt in @("E:\esp\.espressif", (Join-Path $env:USERPROFILE ".espressif"))) {
        if (Test-Path (Join-Path $alt "python_env")) { $ToolsPath = $alt; break }
    }
}

$env:IDF_PATH       = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath

# ---------------------------------------------------------------- 硬绑定 venv（同 idf.ps1）
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
    Write-Host "    请先运行：.\\tools\\install_esp_idf.ps1" -ForegroundColor Yellow
    Write-Host ""
    return
}

$env:IDF_PYTHON_ENV_PATH = $venvDir
$env:PATH = (Split-Path $venvPython) + ";" + $env:PATH

if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    Write-Host ""
    Write-Host "[X] 未找到 ESP-IDF 激活脚本：$(Join-Path $IdfPath 'export.ps1')" -ForegroundColor Red
    Write-Host "    请先运行：.\\tools\\install_esp_idf.ps1" -ForegroundColor Yellow
    Write-Host ""
    return
}

# ---------------------------------------------------------------- 激活
. (Join-Path $IdfPath "export.ps1") | Out-Null

if (Get-Command idf.py -ErrorAction SilentlyContinue) {
    $ver = (idf.py --version 2>$null) -join ''
    Write-Host ""
    Write-Host "[OK] ESP-IDF 环境已就绪：$ver" -ForegroundColor Green
    Write-Host "     可直接敲 idf.py build / idf.py -p COM7 flash monitor" -ForegroundColor DarkGray
    Write-Host ""
} else {
    Write-Host ""
    Write-Host "[X] ESP-IDF 激活失败，请运行 .\\tools\\check_env.ps1 查看哪一项是 FAIL" -ForegroundColor Red
    Write-Host ""
}
