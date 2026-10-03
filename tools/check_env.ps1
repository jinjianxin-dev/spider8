<#
    Spider8 项目 —— 开发环境自检脚本
    用法（PowerShell）：
        .\tools\check_env.ps1
        .\tools\check_env.ps1 -InstallRoot E:\esp

    只读检查，不修改任何环境。
#>
[CmdletBinding()]
param(
    [string]$InstallRoot = "E:\esp"
)

$script:pass = 0
$script:warn = 0
$script:fail = 0

function Write-Item([string]$name, [string]$status, [string]$detail) {
    switch ($status) {
        "OK"   { $script:pass++; $color = "Green"  ; $tag = "[ OK ]" }
        "WARN" { $script:warn++; $color = "Yellow" ; $tag = "[WARN]" }
        "FAIL" { $script:fail++; $color = "Red"    ; $tag = "[FAIL]" }
    }
    Write-Host ("  {0} {1,-26} {2}" -f $tag, $name, $detail) -ForegroundColor $color
}

Write-Host ""
Write-Host "==================================================" -ForegroundColor White
Write-Host " Spider8 开发环境自检" -ForegroundColor White
Write-Host "==================================================" -ForegroundColor White

# ------------------------------------------------ 1. 基础工具
Write-Host ""
Write-Host "[1] 基础工具链" -ForegroundColor Cyan

$gitV = $null
try { $gitV = (git --version 2>&1) -join '' } catch { }
if ($gitV -match 'git version') { Write-Item "Git" "OK" $gitV }
else { Write-Item "Git" "FAIL" "未安装或不在 PATH" }

$pyV = $null
$pyExe = $null
foreach ($c in @("python", "python3")) {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $pyExe = $c; break }
}
if ($null -ne $pyExe) {
    $pyV = (& $pyExe --version 2>&1) -join ''
    if ($pyV -match 'Python 3\.(\d+)') {
        $minor = [int]$Matches[1]
        if ($minor -ge 9 -and $minor -le 13) {
            Write-Item "Python" "OK" "$pyV （在 IDF 支持的 3.9~3.13 范围内）"
        } elseif ($minor -gt 13) {
            Write-Item "Python" "WARN" "$pyV 超出 IDF 5.5 支持范围(3.9~3.13)，装依赖可能失败"
        } else {
            Write-Item "Python" "WARN" "$pyV 版本过低（需 3.9+）"
        }
    } else {
        Write-Item "Python" "WARN" "$pyV （无法识别版本）"
    }
    Write-Host "      路径: $((Get-Command $pyExe).Source)" -ForegroundColor Gray
} else {
    Write-Item "Python" "FAIL" "未安装或不在 PATH"
}

# ------------------------------------------------ 2. ESP-IDF 安装
Write-Host ""
Write-Host "[2] ESP-IDF 安装" -ForegroundColor Cyan

$IdfPath   = Join-Path $InstallRoot "esp-idf"
$ToolsPath = Join-Path $InstallRoot ".espressif"

if (Test-Path (Join-Path $IdfPath "install.ps1")) { Write-Item "ESP-IDF 源码" "OK" $IdfPath }
else { Write-Item "ESP-IDF 源码" "FAIL" "未找到 $IdfPath（请先运行 install_esp_idf.ps1）" }

if (Test-Path $ToolsPath) { Write-Item "工具链目录" "OK" $ToolsPath }
else { Write-Item "工具链目录" "FAIL" "未找到 $ToolsPath" }

if ($env:IDF_PATH) { Write-Item "IDF_PATH (当前会话)" "OK" $env:IDF_PATH }
else { Write-Item "IDF_PATH (当前会话)" "WARN" "未激活，请先执行 $IdfPath\export.ps1" }

# ------------------------------------------------ 3. 编译工具
Write-Host ""
Write-Host "[3] 编译与烧写工具" -ForegroundColor Cyan

$idfPy = Get-Command idf.py -ErrorAction SilentlyContinue
if ($null -ne $idfPy) {
    $idfVer = (idf.py --version 2>&1) -join ''
    Write-Item "idf.py" "OK" $idfVer
} else {
    Write-Item "idf.py" "WARN" "当前会话未激活环境（先跑 export.ps1）"
}

$gcc = Get-Command xtensa-esp32s3-elf-gcc -ErrorAction SilentlyContinue
if ($null -ne $gcc) {
    $gccVer = (xtensa-esp32s3-elf-gcc --version 2>&1 | Select-Object -First 1) -join ''
    Write-Item "xtensa-esp32s3-elf-gcc" "OK" $gccVer
} else {
    Write-Item "xtensa-esp32s3-elf-gcc" "WARN" "未找到（未激活环境或工具链未装）"
}

# ------------------------------------------------ 4. 串口设备
Write-Host ""
Write-Host "[4] 串口设备（板子插上应能看到 COMx）" -ForegroundColor Cyan
try {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -gt 0) {
        Write-Item "可用串口" "OK" ($ports -join ', ')
        Write-Host "      提示：ESP32-S3 通常显示为 USB-SERIAL CH343 / CP210x / USB JTAG" -ForegroundColor Gray
    } else {
        Write-Item "可用串口" "WARN" "未检测到串口（板子未插 / 缺驱动 / 用了充电线）"
    }
} catch {
    Write-Item "可用串口" "WARN" "枚举失败: $_"
}

# ------------------------------------------------ 5. 磁盘空间
Write-Host ""
Write-Host "[5] 磁盘空间" -ForegroundColor Cyan
foreach ($d in @("C", "D", "E")) {
    $disk = Get-PSDrive -Name $d -ErrorAction SilentlyContinue
    if ($null -ne $disk) {
        $free = [math]::Round($disk.Free / 1GB, 1)
        if ($free -lt 5) { Write-Item "${d}: 盘可用空间" "WARN" "$free GB（偏少）" }
        else { Write-Item "${d}: 盘可用空间" "OK" "$free GB" }
    }
}

# ------------------------------------------------ 汇总
Write-Host ""
Write-Host "==================================================" -ForegroundColor White
Write-Host (" 汇总：OK {0} 项 / WARN {1} 项 / FAIL {2} 项" -f $script:pass, $script:warn, $script:fail) -ForegroundColor White
Write-Host "==================================================" -ForegroundColor White
if ($script:fail -eq 0 -and $script:warn -eq 0) {
    Write-Host " 环境就绪，可以进入 M0 开发。" -ForegroundColor Green
} elseif ($script:fail -eq 0) {
    Write-Host " 基本可用，WARN 项按 docs/03-环境搭建.md 第 8 节排查。" -ForegroundColor Yellow
} else {
    Write-Host " 存在 FAIL 项，请按 docs/03-环境搭建.md 修复后重跑本脚本。" -ForegroundColor Red
}
Write-Host ""
