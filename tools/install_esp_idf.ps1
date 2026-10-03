<#
    Spider8 项目 —— ESP-IDF 一键安装脚本（国内镜像加速版）
    适用：Windows 10/11 + Windows PowerShell 5.1+

    用法：
        Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
        .\tools\install_esp_idf.ps1

    可选参数：
        -InstallRoot E:\esp        安装根目录（默认 E:\esp）
        -IdfVersion  v5.5.5        ESP-IDF 版本
        -ToolsPath   E:\esp\.espressif   工具链目录

    说明：脚本只做安装，不修改系统全局 PATH。每次开发用 export.ps1 激活环境。
#>
[CmdletBinding()]
param(
    [string]$InstallRoot = "E:\esp",
    [string]$IdfVersion  = "v5.5.5",
    [string]$Mirror      = "https://gitee.com/EspressifSystems/esp-idf.git",
    [string]$PythonExe   = ""     # 可选：手动指定用于安装的 python.exe（推荐 3.11/3.13）
)

$ErrorActionPreference = "Stop"

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}
function Write-Ok([string]$msg)   { Write-Host "    [OK]  $msg" -ForegroundColor Green }
function Write-Warn2([string]$msg){ Write-Host "    [!!]  $msg" -ForegroundColor Yellow }
function Write-Err([string]$msg)  { Write-Host "    [X]   $msg" -ForegroundColor Red }

Write-Host "==============================================" -ForegroundColor White
Write-Host " Spider8 - ESP-IDF 安装脚本" -ForegroundColor White
Write-Host " 版本: $IdfVersion   安装到: $InstallRoot" -ForegroundColor White
Write-Host "==============================================" -ForegroundColor White

# ---------------------------------------------------------------- 1. 前置检查
Write-Step "1/6 前置条件检查"

$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($null -eq $gitCmd) {
    Write-Err "未找到 git，请先安装 Git for Windows：https://git-scm.com/download/win"
    exit 1
}
Write-Ok "git: $((git --version) -join '')"

# --- Python 选择：ESP-IDF 5.5 官方支持 Python 3.9 ~ 3.13，3.14+ 装依赖大概率失败 ---
function Get-PyVersion([string]$exe) {
    try { return ((& $exe --version 2>&1) -join '') } catch { return "unknown" }
}
function Test-PySupported([string]$verStr) {
    if ($verStr -match 'Python 3\.(\d+)') {
        $minor = [int]$Matches[1]
        if ($minor -ge 9 -and $minor -le 13) { return $true }
    }
    return $false
}

$pyExe = $null
$pyVer = $null

if ($PythonExe -ne "") {
    if (-not (Test-Path $PythonExe)) {
        Write-Err "指定的 -PythonExe 不存在：$PythonExe"
        exit 1
    }
    $pyExe = $PythonExe
    $pyVer = Get-PyVersion $pyExe
    if (-not (Test-PySupported $pyVer)) {
        Write-Err "指定的 Python 版本不受支持：$pyVer（需要 3.9~3.13）"
        exit 1
    }
    Write-Ok "使用指定 Python: $pyVer"
} else {
    # 优先在 PATH 中找受支持的版本
    $candidates = @()
    foreach ($cand in @("python", "python3", "py")) {
        $c = Get-Command $cand -ErrorAction SilentlyContinue
        if ($null -ne $c) { $candidates += $c.Source }
    }
    # 再补充常见安装位置
    $candidates += @(
        "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        "C:\Users\$env:USERNAME\.workbuddy\binaries\python\versions\3.13.12\python.exe",
        "C:\Python313\python.exe",
        "C:\Python311\python.exe"
    )
    $pathVersions = @()
    foreach ($exe in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path $exe)) { continue }
        $v = Get-PyVersion $exe
        if (Test-PySupported $v) {
            $pyExe = $exe
            $pyVer = $v
            break
        } else {
            $pathVersions += "$exe => $v"
        }
    }
    if ($null -eq $pyExe) {
        Write-Err "未找到受支持的 Python（需要 3.9~3.13）"
        if ($pathVersions.Count -gt 0) {
            Write-Warn2 "已发现的其它版本（不受支持）："
            $pathVersions | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkGray }
        }
        Write-Host "    解决办法（任选其一）：" -ForegroundColor White
        Write-Host "      1) 安装 Python 3.13：https://www.python.org/downloads/release/python-31312/" -ForegroundColor Gray
        Write-Host "         安装时勾选 Add python.exe to PATH" -ForegroundColor Gray
        Write-Host "      2) 若本机已有其它版本，用参数指定：" -ForegroundColor Gray
        Write-Host "         .\tools\install_esp_idf.ps1 -PythonExe C:\Path\to\python.exe" -ForegroundColor Gray
        exit 1
    }
    Write-Ok "自动选用 Python: $pyVer  ($pyExe)"
}

# 把选中的 Python 目录提到 PATH 最前，确保 install.ps1 用的是它
$pyDir = Split-Path $pyExe -Parent
$env:PATH = "$pyDir;$env:PATH"
Write-Ok "已将该 Python 提升至 PATH 首位，install.ps1 将使用它"

# 磁盘空间检查（需 >= 6GB）
$drive = (Split-Path -Qualifier $InstallRoot)
$disk = Get-PSDrive -Name $drive.TrimEnd(':') -ErrorAction SilentlyContinue
if ($null -ne $disk) {
    $freeGB = [math]::Round($disk.Free / 1GB, 1)
    if ($freeGB -lt 6) {
        Write-Err "$drive 盘剩余仅 $freeGB GB，建议 >= 6GB，请更换 -InstallRoot 参数"
        exit 1
    }
    Write-Ok "$drive 盘剩余空间: $freeGB GB"
}

$IdfPath   = Join-Path $InstallRoot "esp-idf"
$ToolsPath = Join-Path $InstallRoot ".espressif"
$env:IDF_TOOLS_PATH = $ToolsPath

# ---------------------------------------------------------------- 2. 创建目录
Write-Step "2/6 准备安装目录"
if (-not (Test-Path $InstallRoot)) {
    New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
    Write-Ok "已创建 $InstallRoot"
} else {
    Write-Ok "已存在 $InstallRoot"
}
if (-not (Test-Path $ToolsPath)) {
    New-Item -ItemType Directory -Path $ToolsPath -Force | Out-Null
}
Write-Ok "IDF_TOOLS_PATH = $ToolsPath"

# ---------------------------------------------------------------- 3. 克隆源码
Write-Step "3/6 克隆 ESP-IDF $IdfVersion（Gitee 镜像，浅克隆加快速度）"
if (Test-Path (Join-Path $IdfPath "install.ps1")) {
    Write-Warn2 "$IdfPath 已存在且看起来是完整仓库，跳过克隆"
    Write-Warn2 "如需重装请先备份后手动删除该目录"
} else {
    Write-Host "    git clone -b $IdfVersion --depth 1 --shallow-submodules $Mirror"
    git clone -b $IdfVersion --depth 1 --shallow-submodules $Mirror $IdfPath
    if ($LASTEXITCODE -ne 0) {
        Write-Err "克隆失败。可尝试：1) 检查网络 2) 手动 git clone 后再运行本脚本"
        exit 1
    }
    Write-Ok "源码克隆完成"
}

# ---------------------------------------------------------------- 4. 安装工具链
Write-Step "4/6 安装 esp32s3 工具链（下载约 1.5GB，请耐心等待）"
Push-Location $IdfPath
try {
    # 让 git 能识别浅克隆的子模块
    git config --global --add safe.directory ($IdfPath -replace '\\', '/')
    & "$IdfPath\install.ps1" esp32s3
    if ($LASTEXITCODE -ne 0) {
        Write-Err "工具链安装失败（退出码 $LASTEXITCODE）。常见原因：网络中断、杀毒软件拦截"
        exit 1
    }
} finally {
    Pop-Location
}
Write-Ok "工具链安装完成"

# ---------------------------------------------------------------- 5. 激活并验证
Write-Step "5/6 激活环境并验证 idf.py"
& "$IdfPath\export.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Err "export.ps1 执行失败"
    exit 1
}
$idfVer = (idf.py --version 2>&1) -join ''
Write-Ok "idf.py --version => $idfVer"

$gcc = Get-Command xtensa-esp32s3-elf-gcc -ErrorAction SilentlyContinue
if ($null -ne $gcc) {
    Write-Ok "xtensa-esp32s3-elf-gcc 可用"
} else {
    Write-Warn2 "未在当前会话找到 xtensa-esp32s3-elf-gcc（重开终端后再跑 check_env.ps1 确认）"
}

# ---------------------------------------------------------------- 6. 使用说明
Write-Step "6/6 安装完成，使用方式"
$shortcutCmd = "powershell -ExecutionPolicy Bypass -NoExit -Command `"$IdfPath\export.ps1`""
Write-Host "    每次开发先激活环境（新开 PowerShell 窗口执行）：" -ForegroundColor White
Write-Host "      $IdfPath\export.ps1" -ForegroundColor Gray
Write-Host ""
Write-Host "    或创建桌面快捷方式，目标填：" -ForegroundColor White
Write-Host "      $shortcutCmd" -ForegroundColor Gray
Write-Host ""
Write-Host "    验证安装是否完整：" -ForegroundColor White
Write-Host "      .\tools\check_env.ps1" -ForegroundColor Gray
Write-Host ""
Write-Host "  安装全部完成。" -ForegroundColor Green
