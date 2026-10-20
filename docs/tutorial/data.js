/* ==========================================================================
   Spider8 开发手册 · 内容数据
   所有命令、报错、日志均来自本机真实记录（2026-10-02 ~ 2026-10-03）
   ========================================================================== */

window.CHAPTERS = [

/* ------------------------------------------------------------------ 00 */
{
  id: 'overview',
  num: '00',
  title: '开始之前',
  subtitle: '这份教程带你走完什么路',
  html: String.raw`
<p class="lead">这不是一份「照着敲就能跑」的流水账。它记录的是这台机器上<strong>真实踩过的 7 个坑</strong>——每个坑都写了现象、报错原文、根因和验证方式。照着走一遍，你不用再踩第二次。</p>

<h2>覆盖范围</h2>
<div class="cards">
  <div class="card"><span class="k">01</span><h4>装环境</h4><p>ESP-IDF v5.5.5 + ESP32-S3 工具链。装到 E 盘，避开 C 盘空间陷阱。</p></div>
  <div class="card"><span class="k">02</span><h4>配 VS Code</h4><p>7 个一键任务，零扩展依赖，绕开默认终端的 -NoExit 陷阱。</p></div>
  <div class="card"><span class="k">03</span><h4>编译烧写</h4><p>1079 个编译单元、202 KB 固件、4 个镜像的烧写布局。</p></div>
  <div class="card"><span class="k">04</span><h4>验收</h4><p>逐行读懂串口日志，用判定器自动核对板子真实规格。</p></div>
  <div class="card"><span class="k">05</span><h4>命令详解</h4><p>idf.py 每个子命令在干什么，参数为什么这么填。</p></div>
  <div class="card"><span class="k">06</span><h4>错误处理</h4><p>7 个真实故障：现象、原文、根因、修复、验证。</p></div>
</div>

<h2>硬件清单</h2>
<div class="table-wrap">
<table>
<thead><tr><th>物料</th><th>规格</th><th>用途</th><th>阶段</th></tr></thead>
<tbody>
<tr><td>开发板</td><td>ESP32-S3-N16R8（16 MB Flash + 8 MB Octal PSRAM）</td><td>主控，板载原生 USB-Serial/JTAG</td><td><span class="tag green">必需</span></td></tr>
<tr><td>USB 线</td><td><strong>数据线</strong>（不是充电线）</td><td>烧写与串口日志</td><td><span class="tag green">必需</span></td></tr>
<tr><td>舵机</td><td>MG90S 金属齿轮 × 8</td><td>四条腿 × 每腿 2 关节</td><td><span class="tag amber">M0-2 起</span></td></tr>
<tr><td>超声波</td><td>HC-SR04 × 1</td><td>避障测距</td><td><span class="tag blue">M4</span></td></tr>
<tr><td>结构件</td><td>乐高 Technic 积木</td><td>改结构零等待，迭代最快</td><td><span class="tag blue">M2 起</span></td></tr>
<tr><td>电源</td><td>2S 18650 + 5V/5A 降压模块</td><td>舵机独立供电轨，堵转不拖垮主控</td><td><span class="tag blue">M2 起</span></td></tr>
</tbody>
</table>
</div>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">只有开发板也能走完本教程</span>
    <p>M0 阶段全部验收都不需要舵机。想提前验证 PWM 输出，把 <strong>LED + 220Ω 电阻</strong>接到舵机信号脚（GPIO4），50 Hz 下亮度会随脉宽明显变化——肉眼就能确认通道正常。</p>
  </div>
</div>

<h2>这台机器的实测环境</h2>
<p>下面每一项都是实际探测结果，不是"推荐配置"。你换一台机器，值会不同，但<strong>检查方法完全一样</strong>。</p>
<div class="table-wrap">
<table>
<thead><tr><th>检查项</th><th>实测值</th><th>结论</th></tr></thead>
<tbody>
<tr><td>操作系统</td><td>Windows（win32）</td><td><span class="tag green">OK</span></td></tr>
<tr><td>Git</td><td>2.55.0.windows.3</td><td><span class="tag green">OK</span></td></tr>
<tr><td>Python（PATH 默认）</td><td>3.14.6</td><td><span class="tag red">超出 IDF 5.5 支持范围</span></td></tr>
<tr><td>Python（备用）</td><td>3.13.12</td><td><span class="tag green">安装脚本自动选用这个</span></td></tr>
<tr><td>ESP-IDF</td><td>v5.5.5 → <code>E:\esp\esp-idf</code></td><td><span class="tag green">已安装</span></td></tr>
<tr><td>工具链</td><td><code>E:\esp\.espressif</code></td><td><span class="tag green">已安装</span></td></tr>
<tr><td>串口</td><td><code>COM7</code>（USB 串行设备）</td><td><span class="tag green">板子已识别</span></td></tr>
<tr><td>C 盘剩余</td><td>8.2 GB</td><td><span class="tag amber">不要装 C 盘</span></td></tr>
<tr><td>E 盘剩余</td><td>97.8 GB</td><td><span class="tag green">推荐安装位置</span></td></tr>
</tbody>
</table>
</div>

<h2>里程碑路线</h2>
<div class="table-wrap">
<table>
<thead><tr><th>节点</th><th>目标</th><th>验收标志</th><th>状态</th></tr></thead>
<tbody>
<tr><td><strong>M0-1</strong></td><td>固件工程骨架 + 链路打通</td><td>串口可见横幅 + 1 Hz 心跳</td><td><span class="tag green">已完成</span></td></tr>
<tr><td>M0-2</td><td>8 路舵机 LEDC 驱动 + 校准框架</td><td>8 路 50 Hz PWM 可单独点动</td><td><span class="tag amber">下一步</span></td></tr>
<tr><td>M1</td><td>单腿校准</td><td>每关节中位/限位落 NVS</td><td>待开始</td></tr>
<tr><td>M2</td><td>站立</td><td>四腿支撑稳定不抖</td><td>待开始</td></tr>
<tr><td>M3</td><td>遥控爬行</td><td>BLE 摇杆控制波步前进</td><td>待开始</td></tr>
<tr><td>M4</td><td>自动避障</td><td>遇障停止并转向</td><td>待开始</td></tr>
<tr><td>M5</td><td>动作库</td><td>挥手/趴下/跳舞等组合动作</td><td>待开始</td></tr>
<tr><td>M6</td><td>外观与联调</td><td>整机连续运行 30 分钟无异常</td><td>待开始</td></tr>
</tbody>
</table>
</div>

<h2>怎么用这份教程</h2>
<ul>
  <li><strong>目录</strong>：左上按钮（窄屏）或左侧栏，滚动时自动高亮当前小节。</li>
  <li><strong>进度</strong>：每节末尾有「标记本节已完成」，存在浏览器本地，关掉再开还在。</li>
  <li><strong>复制</strong>：代码块右上角一键复制，不用手动框选。</li>
  <li><strong>查错</strong>：出问题时直接去 <a href="#troubleshoot">第 06 章</a>，在搜索框里粘进报错关键字（例如 <code>py3.14_env</code>）。</li>
  <li><strong>验收</strong>：<a href="#accept">第 04 章</a> 有日志判定器，把串口输出整段粘进去，自动核对板子规格。</li>
</ul>

<div class="callout ok">
  <span class="ico">✓</span>
  <div>
    <span class="ttl">预计总耗时</span>
    <p>装环境 15~30 分钟（主要花在下载工具链）· VS Code 配置 5 分钟 · 首次编译 1~5 分钟 · 烧写与验收 5 分钟。</p>
  </div>
</div>
`
},

/* ------------------------------------------------------------------ 01 */
{
  id: 'setup',
  num: '01',
  title: '环境搭建',
  subtitle: '装 ESP-IDF，并把安装位置选对',
  html: String.raw`
<p class="lead">这一节的目标只有一个：让 <code>idf.py --version</code> 能输出 <code>ESP-IDF v5.5.5</code>。但在此之前，有三件事必须先看清楚，否则后面全是玄学错误。</p>

<h2>先体检，不要盲目装</h2>
<p>盲目开装的下场通常是：下到一半 C 盘满了、装依赖卡在 Python 3.14 的轮子上、装完发现串口认不出来。三条命令，两分钟，先把风险排掉。</p>

<div class="code" data-lang="powershell"><pre><code># 1) 磁盘剩余空间（C 盘吃紧是很多 Windows 机器的常态）
Get-CimInstance Win32_LogicalDisk -Filter "DeviceType=3" |
  Select-Object DeviceID, @{n='FreeGB';e={[math]::Round($_.FreeSpace/1GB,1)}}

# 2) PATH 里默认的 python 是哪个版本
python --version

# 3) 板子插上后能看到哪些串口
[System.IO.Ports.SerialPort]::GetPortNames()</code></pre></div>

<div class="callout warn">
  <span class="ico">!</span>
  <div>
    <span class="ttl">为什么这三项必须先看</span>
    <p><strong>磁盘</strong>：ESP-IDF 源码约 1.2 GB，工具链 + venv 约 1.5~2 GB，再加编译缓存，合计 3 GB 起步。C 盘只剩几 GB 时不只是"装不下"——空间归零后 Windows 连删除文件都会失败（删除走回收站，回收站也需要空间）。</p>
    <p><strong>Python</strong>：ESP-IDF 5.5 官方支持 3.9 ~ 3.13。PATH 里如果是 3.14，装依赖时会在构建轮子（wheel）环节失败，而且报错信息完全看不出是版本问题。</p>
    <p><strong>串口</strong>：串口号不是固定值，换 USB 口就可能变。烧写命令必须填对。</p>
  </div>
</div>

<h2>安装路径规划</h2>
<p>ESP-IDF 对路径有硬性要求：<strong>全英文、无空格、无中文</strong>。中文路径会在 CMake/Ninja 阶段炸出看不懂的乱码错误。</p>

<div class="code" data-lang="text"><pre><code>E:\esp\
├── esp-idf\          ESP-IDF v5.5.5 源码（约 1.2 GB）
└── .espressif\       工具链 + Python 虚拟环境（约 1.5~2 GB）

C:\Users\aaron\WorkBuddy\spider8\
├── firmware\         固件工程（编译产物在 firmware\build\）
├── tools\            安装/自检/环境包装脚本
└── docs\             设计与里程碑文档</code></pre></div>

<h2>一键安装</h2>
<ol class="steps">
  <li>
    <span class="st-t">打开 Windows PowerShell</span>
    <span class="st-d">不是 CMD。<kbd>Win</kbd> + <kbd>X</kbd> → 终端 / Windows PowerShell。</span>
  </li>
  <li>
    <span class="st-t">放开执行策略（只对当前窗口生效）</span>
    <span class="st-d">PowerShell 默认禁止运行未签名脚本，不放开会报「无法加载文件，因为在此系统上禁止运行脚本」。</span>
    <div class="code" data-lang="powershell"><pre><code>Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass</code></pre></div>
  </li>
  <li>
    <span class="st-t">运行安装脚本</span>
    <div class="code" data-lang="powershell"><pre><code>cd C:\Users\aaron\WorkBuddy\spider8
.\tools\install_esp_idf.ps1</code></pre></div>
  </li>
  <li>
    <span class="st-t">等 15~30 分钟</span>
    <span class="st-d">时间几乎全花在下载工具链上（约 1.5 GB）。看到 <code>idf.py --version =&gt; ESP-IDF v5.5.5</code> 就是成功了。</span>
  </li>
  <li>
    <span class="st-t">关掉窗口重开一个</span>
    <span class="st-d">环境变量只在当前会话生效过，重开一个干净窗口再跑自检，结果才可信。</span>
  </li>
</ol>

<h3>脚本参数</h3>
<div class="table-wrap">
<table>
<thead><tr><th>参数</th><th>默认值</th><th>什么时候改</th></tr></thead>
<tbody>
<tr><td><code>-InstallRoot</code></td><td><code>E:\esp</code></td><td>E 盘空间不足时换盘（<strong>别换到 C 盘</strong>）</td></tr>
<tr><td><code>-IdfVersion</code></td><td><code>v5.5.5</code></td><td>需要特定版本时。5.5 是成熟稳定线，社区例程最多</td></tr>
<tr><td><code>-Mirror</code></td><td>Gitee ESP-IDF 镜像</td><td>镜像不可用时换官方源或其它镜像</td></tr>
<tr><td><code>-PythonExe</code></td><td>自动挑选 3.9~3.13</td><td>自动挑选失败时手动指定，例如 <code>-PythonExe "C:\...\3.13.12\python.exe"</code></td></tr>
</tbody>
</table>
</div>

<details>
  <summary>脚本内部做了哪 6 步</summary>
  <div class="body">
    <div class="table-wrap">
      <table>
      <thead><tr><th>步骤</th><th>动作</th><th>失败会怎样</th></tr></thead>
      <tbody>
      <tr><td>1/6 前置检查</td><td>查 git；筛出 3.9~3.13 的 python</td><td>直接退出，不会留下半装状态</td></tr>
      <tr><td>2/6 克隆源码</td><td>Gitee 镜像拉 v5.5.5</td><td>网络问题，可重跑（已存在的会跳过）</td></tr>
      <tr><td>3/6 装工具链</td><td>下载 esp32s3 交叉编译器、esptool、OpenOCD</td><td>最慢的一步，超时通常要挂代理</td></tr>
      <tr><td>4/6 建 venv</td><td>在 <code>.espressif\python_env</code> 建虚拟环境</td><td>Python 版本不对会在这里炸</td></tr>
      <tr><td>5/6 设置目标</td><td>锁定 <code>esp32s3</code></td><td>—</td></tr>
      <tr><td>6/6 校验</td><td>跑一次 <code>idf.py --version</code></td><td>失败说明激活链路有问题</td></tr>
      </tbody>
      </table>
    </div>
  </div>
</details>

<h2>镜像的选择，与一个隐藏很深的坑</h2>
<p>本机访问 <code>github.com</code> 首包要 7.2 秒，所以脚本默认走 Gitee 镜像——这没问题。但镜像有一个副作用值得你知道：<strong>Gitee 上的 ESP-IDF 镜像缺了若干子模块仓库</strong>，克隆时不会报错，等到编译才会暴露 <code>Unable to find current revision in submodule path</code>。</p>

<p>而真正的元凶往往不是镜像，是 <strong>git 全局配置里的 URL 改写规则</strong>。本机就有一条：把所有 <code>github.com</code> 访问劫持到已经停服的 <code>ghproxy.com</code>。查一下自己有没有：</p>

<div class="code" data-lang="powershell"><pre><code># 看有没有 insteadOf 改写规则
git config --global --get-regexp insteadof</code></pre></div>

<p>如果输出里有指向 <code>ghproxy.com</code>（或任何已停服代理）的规则，建议清掉。这会影响你<strong>所有</strong>项目的 GitHub 访问，不只是本项目：</p>

<div class="code" data-lang="powershell"><pre><code># 删除那条失效的改写规则
git config --global --unset url.https://ghproxy.com/https://github.com/.insteadof</code></pre></div>

<div class="callout danger">
  <span class="ico">×</span>
  <div>
    <span class="ttl">不要在项目里替别人改全局配置</span>
    <p>上面这条命令会改你机器上<strong>所有</strong>仓库的行为。如果那台机器还在用别的代理方案，先确认再动手。临时绕过只需要给单条命令加环境变量：<code>$env:GIT_CONFIG_GLOBAL="NUL"</code>（PowerShell）或 <code>GIT_CONFIG_GLOBAL=/dev/null</code>（Git Bash）。</p>
  </div>
</div>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">如果已经踩了：子模块修复思路</span>
    <p>先把代理规则绕开，再把 <code>.gitmodules</code> 换成 GitHub 官方版本（镜像改写过的版本地址是坏的），然后 <code>git submodule update --init --recursive --jobs 8</code> 并行拉取。已经克隆坏的那几个，需要先 <code>git submodule deinit -f &lt;路径&gt;</code> 再重拉，否则会一直报找不到目标 commit。</p>
  </div>
</div>

<h2>装完自检</h2>
<p>脚本会做一次校验，但更完整的是项目里的自检脚本——它逐项检查并给出 OK / WARN / FAIL 三态报告：</p>

<div class="code" data-lang="powershell"><pre><code>cd C:\Users\aaron\WorkBuddy\spider8
.\tools\check_env.ps1</code></pre></div>

<p>刚装完 ESP-IDF、<strong>还没激活环境</strong>时，输出长这样（这是真实抓取）：</p>

<div class="code" data-lang="text"><pre><code>==================================================
 Spider8 开发环境自检
==================================================

[1] 基础工具链
  [ OK ] Git                        git version 2.55.0.windows.3
  [WARN] Python                     Python 3.14.6 超出 IDF 5.5 支持范围(3.9~3.13)，装依赖可能失败
      路径: C:\Users\aaron\AppData\Local\Python\bin\python.exe

[2] ESP-IDF 安装
  [ OK ] ESP-IDF 源码              E:\esp\esp-idf
  [ OK ] 工具链目录                 E:\esp\.espressif
  [WARN] IDF_PATH (当前会话)        未激活，请先执行 E:\esp\esp-idf\export.ps1

[3] 编译与烧写工具
  [WARN] idf.py                    当前会话未激活环境（先跑 export.ps1）
  [WARN] xtensa-esp32s3-elf-gcc    未找到（未激活环境或工具链未装）

[4] 串口设备（板子插上应能看到 COMx）
  [ OK ] 可用串口                  COM7

[5] 磁盘空间
  [ OK ] C: 盘可用空间             8.2 GB
  [ OK ] E: 盘可用空间             97.8 GB

==================================================
 汇总：OK 6 项 / WARN 4 项 / FAIL 0 项
==================================================</code></pre></div>

<div class="table-wrap">
<table>
<thead><tr><th>状态</th><th>含义</th><th>要不要处理</th></tr></thead>
<tbody>
<tr><td><span class="tag green">OK</span></td><td>该项正常</td><td>不用管</td></tr>
<tr><td><span class="tag amber">WARN</span></td><td>能用，但有隐患或当前会话未激活</td><td>看具体情况。<code>IDF_PATH</code> / <code>idf.py</code> 的 WARN 在「新开窗口未激活」时是<strong>正常的</strong></td></tr>
<tr><td><span class="tag red">FAIL</span></td><td>缺东西，走不下去</td><td>必须先修，脚本结束时会给出对应文档章节</td></tr>
</tbody>
</table>
</div>

<div class="callout ok">
  <span class="ico">✓</span>
  <div>
    <span class="ttl">怎么算"真的装好了"</span>
    <p>不看自检的 WARN，看这一条：在<strong>项目目录下</strong>执行 <code>.\tools\idf.ps1 --version</code>，输出 <code>ESP-IDF v5.5.5</code>。<code>idf.ps1</code> 会自动激活环境，能跑通就说明整条链路（路径 + venv + 工具链）都通。</p>
  </div>
</div>

<h2>每次开发要不要手动激活？</h2>
<p>ESP-IDF 的环境变量不会全局生效（脚本刻意没改系统 PATH，避免和别的工具打架）。三种激活方式：</p>
<div class="table-wrap">
<table>
<thead><tr><th>方式</th><th>怎么做</th><th>适合场景</th></tr></thead>
<tbody>
<tr><td><strong>自动（推荐）</strong></td><td><code>.\tools\idf.ps1 &lt;子命令&gt;</code> 或 VS Code 任务</td><td>日常开发，<strong>不用记激活这回事</strong></td></tr>
<tr><td>手动激活终端</td><td><code>E:\esp\esp-idf\export.ps1</code></td><td>想在终端里连续敲多条 idf.py 命令</td></tr>
<tr><td>VS Code 专用终端</td><td>终端下拉选 <strong>ESP-IDF Shell</strong></td><td>已启动即激活，见第 02 章</td></tr>
</tbody>
</table>
</div>

<h2>本节验收</h2>
<ul class="checklist">
  <li><label><input type="checkbox" data-ck="s1"><span>E 盘存在 <code>E:\esp\esp-idf</code> 与 <code>E:\esp\.espressif</code><span class="hint">目录结构对，来源就可信</span></span></label></li>
  <li><label><input type="checkbox" data-ck="s2"><span><code>.\tools\idf.ps1 --version</code> 输出 <code>ESP-IDF v5.5.5</code><span class="hint">这一条通过，说明路径 + venv + 工具链全通</span></span></label></li>
  <li><label><input type="checkbox" data-ck="s3"><span><code>.\tools\check_env.ps1</code> 汇总里 FAIL 为 0<span class="hint">WARN 可以留，FAIL 必须清零</span></span></label></li>
  <li><label><input type="checkbox" data-ck="s4"><span>设备管理器能看到 <code>COM7</code><span class="hint">没有"端口"项说明缺驱动，或用的是充电线</span></span></label></li>
  <li><label><input type="checkbox" data-ck="s5"><span>确认过 git 全局没有失效的 insteadOf 代理规则<span class="hint">有就先清掉，否则后面拉子模块会莫名其妙失败</span></span></label></li>
</ul>
`
},

/* ------------------------------------------------------------------ 02 */
{
  id: 'vscode',
  num: '02',
  title: 'VS Code 配置',
  subtitle: '7 个一键任务，以及一个必须绕开的终端陷阱',
  html: String.raw`
<p class="lead">结论先说：<strong>VS Code 完全可以，而且这是 ESP-IDF 官方主推的开发方式</strong>。两条路线不冲突，可以随时切换。</p>

<h2>两条路线</h2>
<div class="table-wrap">
<table>
<thead><tr><th>路线</th><th>依赖</th><th>能力</th><th>建议</th></tr></thead>
<tbody>
<tr><td><strong>A. 零扩展</strong></td><td>只用 VS Code 自带功能</td><td>编译 / 烧写 / 监视 / menuconfig 一键按钮 + 终端 + 语法补全</td><td><span class="tag green">先用这个</span></td></tr>
<tr><td>B. 官方扩展</td><td>装 Espressif IDF 扩展</td><td>图形化状态栏、芯片概览、断点调试</td><td>需要可视化调试时再加</td></tr>
</tbody>
</table>
</div>

<p>两者本质都是调用 <code>idf.py</code>。之所以优先选 A，是因为<strong>任务系统的写法永远有效</strong>——官方扩展最新版已改成 EIM 配置模型（<code>idf.currentSetup</code>），旧的 <code>idf.espIdfPathWin</code> 那套键名已经不存在了，手写配置很容易随版本失效。</p>

<h2>路线 A 的文件构成</h2>
<div class="code" data-lang="text"><pre><code>spider8/
├── .vscode/
│   ├── tasks.json              7 个一键任务（每个自带 options.shell）
│   ├── settings.json           默认终端 = ESP-IDF Shell；隐藏 build 目录
│   └── c_cpp_properties.json   补全指向 compile_commands.json
└── tools/
    ├── idf.ps1                 【任务用】激活环境后转发给 idf.py，跑完即退
    ├── idf_shell.ps1           【终端用】激活后保持会话
    ├── install_esp_idf.ps1     一键安装
    └── check_env.ps1           环境自检</code></pre></div>

<h2>七个任务</h2>
<p>按 <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> → 输入 <code>Run Task</code> 就能看到全部。每个任务实际执行的都是 <code>tools/idf.ps1</code> 加不同参数。</p>

<div class="table-wrap">
<table>
<thead><tr><th>任务名</th><th>快捷键</th><th>底层命令</th><th>作用</th></tr></thead>
<tbody>
<tr><td>编译 (build)</td><td><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>B</kbd></td><td><code>idf.py build</code></td><td>增量编译，改了哪个文件编哪个</td></tr>
<tr><td>烧写 (flash)</td><td>—</td><td><code>idf.py -p COM7 flash</code></td><td>把 4 个镜像写进 Flash</td></tr>
<tr><td>串口监视 (monitor)</td><td>—</td><td><code>idf.py -p COM7 monitor</code></td><td>常驻查看程序日志，<kbd>Ctrl</kbd>+<kbd>]</kbd> 退出</td></tr>
<tr><td>编译+烧写+监视</td><td>—</td><td><code>idf.py -p COM7 flash monitor</code></td><td>一条龙，只弹一次串口窗</td></tr>
<tr><td>设置目标芯片</td><td>—</td><td><code>idf.py set-target esp32s3</code></td><td>换芯片型号（会清空 build）</td></tr>
<tr><td>菜单配置</td><td>—</td><td><code>idf.py menuconfig</code></td><td>全屏 TUI，改 Kconfig 选项</td></tr>
<tr><td>全量清理</td><td>—</td><td><code>idf.py fullclean</code></td><td>删 build 目录，解决"代码没问题但编译报错"</td></tr>
</tbody>
</table>
</div>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">任务的工作目录是 <code>firmware/</code></span>
    <p>所以任务必须在固件工程建好之后才有意义。在 <code>firmware/</code> 不存在时点任务，会提示目录不存在——那是正常的，不是配置错了。</p>
  </div>
</div>

<h2>为什么每个任务都要自带 shell</h2>
<p>这是本项目踩过最隐蔽的一个坑：任务卡在「正在执行中」永不结束，同时还报了一个看起来完全不相关的错。</p>

<h3>现象</h3>
<p>点「烧写」后，终端报：</p>
<div class="code" data-lang="text"><pre><code>ERROR: ESP-IDF Python virtual environment
  "C:\Users\aaron\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe" not found.

管道元素中的"."后面的表达式生成无效的对象。该表达式必须生成命令名称、脚本块或 CommandInfo 对象。
所在位置 E:\esp\esp-idf\export.ps1:27 字符: 3
+ . $idf_exports</code></pre></div>

<p>而且任务永远显示「正在执行中」，停不下来。</p>

<h3>根因：两个 bug 叠在一起</h3>
<p>当时 <code>settings.json</code> 把带 <code>-NoExit</code> 的「ESP-IDF Shell」设成了<strong>默认终端</strong>。VS Code 跑 shell 任务时，如果任务没有单独指定 shell，就会用默认 profile，并把任务命令行<strong>追加到 profile 参数后面</strong>。真实进程命令行变成了：</p>

<div class="code" data-lang="text"><pre><code>powershell.exe -NoExit -ExecutionPolicy Bypass -Command . 'E:\esp\esp-idf\export.ps1'
        -Command powershell -NoProfile -ExecutionPolicy Bypass -File ...\idf.ps1 -Port COM7 flash
                     ↑ 这一整串被当成 export.ps1 的参数，吞掉了</code></pre></div>

<p>于是产生两个后果，正好对应两个现象：</p>
<ul>
  <li><strong>报错</strong>：真正执行的只是 profile 里那句裸的 <code>. export.ps1</code>——环境里没有 <code>IDF_TOOLS_PATH</code>、<code>python</code> 又是 3.14，于是它去找 <code>idf5.5_py3.14_env</code>，而实际装的是 <code>py3.13_env</code>。<strong>跟 idf.ps1 一点关系都没有。</strong></li>
  <li><strong>不结束</strong>：<code>-NoExit</code> 让这个 shell 永不退出，VS Code 就判定任务还在跑。</li>
</ul>

<div class="callout danger">
  <span class="ico">×</span>
  <div>
    <span class="ttl">结论：带 -NoExit 的终端 profile 不能用来跑任务</span>
    <p>这不是 VS Code 的 bug，是配置语义冲突。终端要「保持打开」，任务要「跑完退出」，两者不可能共用同一个 profile。</p>
  </div>
</div>

<h3>修复：任务显式指定 shell</h3>
<p>每个任务都加了 <code>options.shell</code>，并且 <code>command</code> 直接就是脚本路径，不再套一层 powershell：</p>

<div class="code" data-lang="json"><pre><code>{
  "label": "ESP-IDF: 烧写 (flash)",
  "type": "shell",
  "command": "$&#123;workspaceFolder}\\tools\\idf.ps1",
  "args": ["-Port", "$&#123;input:serialPort}", "flash"],
  "options": {
    "cwd": "$&#123;workspaceFolder}/firmware",
    "shell": {
      "executable": "powershell.exe",
      "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
    }
  },
  "presentation": { "showReuseMessage": false }
}</code></pre></div>

<p>实际执行的是 <code>powershell.exe -NoProfile -ExecutionPolicy Bypass -File &lt;idf.ps1&gt; -Port COM7 flash</code>，进程跑完即退出，<strong>与默认终端 profile 完全解耦</strong>。</p>

<div class="table-wrap">
<table>
<thead><tr><th>验证项</th><th>结果</th></tr></thead>
<tbody>
<tr><td>任务命令行方式（cwd = firmware）</td><td><code>Project build complete</code>，22.8 秒后<strong>自行退出</strong>，退出码 0</td></tr>
<tr><td>终端 profile 方式</td><td><code>[OK] ESP-IDF 环境已就绪：ESP-IDF v5.5.5</code>，退出码 0</td></tr>
</tbody>
</table>
</div>

<h3>已经卡住的任务怎么清掉</h3>
<p>任选其一：</p>
<ul>
  <li>点该终端标签右侧的 <strong>🗑 结束终端</strong> 图标</li>
  <li><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> → <code>Tasks: Terminate Task</code> → 选对应任务</li>
  <li>直接关掉终端面板</li>
</ul>

<h2>路线 B：官方扩展</h2>
<p>想要图形化按钮或硬件断点调试时再加装，注意<strong>不要选 Install</strong>——那会再下一份 2 GB 的 IDF。</p>
<div class="table-wrap">
<table>
<thead><tr><th>配置项</th><th>填写值</th></tr></thead>
<tbody>
<tr><td>ESP-IDF 目录</td><td><code>E:\esp\esp-idf</code>（含 <code>export.ps1</code> 的那一层）</td></tr>
<tr><td>Tools 目录</td><td><code>E:\esp\.espressif</code></td></tr>
<tr><td>Python</td><td><code>C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12\python.exe</code></td></tr>
</tbody>
</table>
</div>

<div class="callout warn">
  <span class="ico">!</span>
  <div>
    <span class="ttl">Python 千万别选 PATH 默认的 3.14</span>
    <p>超出 IDF 5.5 支持范围（3.9~3.13），会在依赖检查阶段报错。走「Use existing setup」而不是重新安装。</p>
  </div>
</div>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">断点调试是白送的</span>
    <p>ESP32-S3 自带 <strong>USB-Serial/JTAG</strong>，一根 USB 线就能做硬件断点，不用买调试器。注意调试会占用 USB 口，此时串口监视要改用另一个 UART。</p>
  </div>
</div>

<h2>代码补全</h2>
<p>需要两步：装微软的 <strong>C/C++</strong> 扩展（<code>ms-vscode.cpptools</code>），然后<strong>编译一次</strong>。编译会生成 <code>firmware/build/compile_commands.json</code>，补全和跳转全靠它。</p>
<p class="muted">所以"刚打开 VS Code 没有补全"不是坏了，是还没编译过。</p>

<h2>本节验收</h2>
<ul class="checklist">
  <li><label><input type="checkbox" data-ck="v1"><span>用 VS Code 打开的是 <code>spider8</code> 文件夹（不是 <code>firmware</code>）<span class="hint">任务里的相对路径以工作区根目录为基准</span></span></label></li>
  <li><label><input type="checkbox" data-ck="v2"><span><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> → <code>Run Task</code> 能看到 7 个 <code>ESP-IDF:</code> 任务</span></label></li>
  <li><label><input type="checkbox" data-ck="v3"><span><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>B</kbd> 能触发编译，且跑完<strong>自行退出</strong>（不会卡在"正在执行中"）<span class="hint">这一条就是在验证 options.shell 是否生效</span></span></label></li>
  <li><label><input type="checkbox" data-ck="v4"><span>内置终端显示 <code>(esp-idf)</code> 前缀或能直接执行 <code>idf.py --version</code><span class="hint">终端下拉里选 ESP-IDF Shell</span></span></label></li>
  <li><label><input type="checkbox" data-ck="v5"><span>已装 C/C++ 扩展，编译后 <code>#include</code> 有补全</span></label></li>
</ul>
`
}

,

/* ------------------------------------------------------------------ 03 */
{
  id: 'flash',
  num: '03',
  title: '编译与烧写',
  subtitle: '从源码到板子里跑起来的那 202 KB',
  html: String.raw`
<p class="lead">这一节把固件送进芯片。理解烧写布局之后，你看到 <code>Wrote 207360 bytes</code> 这种数字就不会发怵——每个数字对应分区表里的一段地址。</p>

<h2>工程结构</h2>
<div class="code" data-lang="text"><pre><code>firmware/
├── CMakeLists.txt        工程入口：project(spider8)
├── partitions.csv        16 MB 分区表（双 OTA + littlefs + coredump）
├── sdkconfig.defaults    芯片/PSRAM/FreeRTOS 的默认配置
├── main/
│   ├── CMakeLists.txt    声明 main 组件
│   └── main.c            启动横幅 + 1 Hz 心跳
└── build/                编译产物（.gitignore 里已排除）
    ├── bootloader/bootloader.bin
    ├── partition_table/partition-table.bin
    ├── spider8.bin
    ├── spider8.elf
    └── compile_commands.json   ← 代码补全靠它</code></pre></div>

<h2>分区表：16 MB 怎么切</h2>
<div class="table-wrap">
<table>
<thead><tr><th>分区</th><th>起始地址</th><th>大小</th><th>用途</th></tr></thead>
<tbody>
<tr><td><code>nvs</code></td><td>0x009000</td><td>24 KB</td><td>键值存储：WiFi 校准、用户参数、舵机校准值（M0-2 会写这里）</td></tr>
<tr><td><code>phy_init</code></td><td>0x00f000</td><td>4 KB</td><td>RF 射频校准数据</td></tr>
<tr><td><code>otadata</code></td><td>0x010000</td><td>8 KB</td><td>OTA 状态：当前从哪个槽启动</td></tr>
<tr><td><code>ota_0</code></td><td>0x020000</td><td>3072 KB</td><td>应用槽 A（当前固件在这里）</td></tr>
<tr><td><code>ota_1</code></td><td>0x320000</td><td>3072 KB</td><td>应用槽 B（OTA 升级时写入，失败可回滚）</td></tr>
<tr><td><code>littlefs</code></td><td>0x620000</td><td>10048 KB</td><td>文件系统：动作库 JSON、配置、日志</td></tr>
<tr><td><code>coredump</code></td><td>0xff0000</td><td>64 KB</td><td>崩溃现场，可用 <code>idf.py coredump-info</code> 还原</td></tr>
</tbody>
</table>
</div>

<p>两个设计取舍值得说明：</p>
<ul>
  <li><strong>双 OTA 槽</strong>：每槽 3 MB，是应用分区的常规做法。当前固件 202 KB，余量 93%。以后要无线升级时，新固件写进另一个槽，验证通过再切换——失败能自动回滚。</li>
  <li><strong>littlefs 占 10 MB</strong>：动作库是 JSON 关键帧，还要存校准表、可能的录音/表情数据。宁可现在留够，也不要以后重新分区（改分区表要整片擦除）。</li>
</ul>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">分区表是"数据"，不是"代码"</span>
    <p>所以启动横幅里的分区列表是<strong>遍历查询</strong>出来的，不是硬编码打印的。改完 <code>partitions.csv</code> 重新编译，日志会自动跟着变——不会出现"改了配置但日志还报旧值"的谎言。</p>
  </div>
</div>

<h2>编译</h2>
<p>在 VS Code 里按 <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>B</kbd>，或命令行：</p>
<div class="code" data-lang="powershell"><pre><code>cd C:\Users\aaron\WorkBuddy\spider8\firmware
..\tools\idf.ps1 build</code></pre></div>

<p>首次编译要编 1079 个单元（协议栈、WiFi、BLE、驱动全都算在内），1~5 分钟；之后改一个文件通常 20 秒左右。成功时末尾是：</p>
<div class="code" data-lang="text"><pre><code>Project build complete. To flash, run:
 idf.py flash
or
 idf.py -p PORT flash monitor</code></pre></div>

<div class="callout warn">
  <span class="ico">!</span>
  <div>
    <span class="ttl">"代码看着没问题但编译报错"</span>
    <p>九成是增量缓存脏了。跑一次 <code>idf.py fullclean</code>（任务：<strong>全量清理</strong>）再编译。它会删掉整个 <code>build/</code>，不会碰源码。</p>
  </div>
</div>

<h2>串口：先让电脑看见板子</h2>
<p>插上板子后，设备管理器 → <strong>端口 (COM 和 LPT)</strong>，应该看到 <code>USB 串行设备 (COM7)</code>。</p>
<p>ESP32-S3 开发板通常有两种 USB 方案，先确认板子上芯片的丝印：</p>
<div class="table-wrap">
<table>
<thead><tr><th>丝印</th><th>驱动</th><th>说明</th></tr></thead>
<tbody>
<tr><td>CH343 / CH9102</td><td>WCH 官网 CH343SER</td><td>USB 转串口芯片，需装驱动</td></tr>
<tr><td>CP2102 / CP2104</td><td>Silicon Labs VCP 驱动</td><td>需装驱动</td></tr>
<tr><td><strong>USB-Serial/JTAG</strong></td><td>Win10/11 免驱</td><td>ESP32-S3 原生 USB，本项目用的就是这个</td></tr>
</tbody>
</table>
</div>

<div class="callout danger">
  <span class="ico">×</span>
  <div>
    <span class="ttl">最常见的"板子坏了"其实是充电线</span>
    <p>很多 USB 线只有电源线、没有数据线。表现是：板子灯亮、但设备管理器里什么都没有。换一根能传数据的线即可。</p>
    <p>另外 ESP32-S3 板子常有两个 USB 口：<strong>标着 USB 的那个</strong>是原生口（免驱、可调试），标着 UART 的那个走 CH340 之类的转接芯片。插错口可能连 COM 号都分不到。</p>
  </div>
</div>

<h2>烧写：四个镜像各就各位</h2>
<div class="code" data-lang="powershell"><pre><code># 任务「ESP-IDF: 烧写 (flash)」等价于：
..\tools\idf.ps1 -Port COM7 flash</code></pre></div>

<p>成功时输出四行 <code>Hash of data verified.</code>，每个镜像落在不同地址：</p>
<div class="code" data-lang="text"><pre><code>Wrote 22272 bytes  (bootloader)    at 0x00000000 ... Hash of data verified.
Wrote 3072 bytes   (partition)     at 0x00008000 ... Hash of data verified.
Wrote 8192 bytes   (boot_app0)     at 0x00010000 ... Hash of data verified.
Wrote 207360 bytes (spider8.bin)   at 0x00020000 ... Hash of data verified.
Leaving... Hard resetting via RTS pin... Done</code></pre></div>

<div class="table-wrap">
<table>
<thead><tr><th>镜像</th><th>落到哪</th><th>干什么用</th></tr></thead>
<tbody>
<tr><td><code>bootloader.bin</code></td><td>0x0</td><td>二级引导程序：初始化 Flash/PSRAM，决定从哪个 OTA 槽启动</td></tr>
<tr><td><code>partition-table.bin</code></td><td>0x8000</td><td>分区表本身，必须固定在这里</td></tr>
<tr><td><code>boot_app0.bin</code></td><td>0x10000</td><td>OTA 数据初值，告诉引导程序"从 ota_0 启动"</td></tr>
<tr><td><code>spider8.bin</code></td><td>0x20000</td><td>你的应用，正好是 <code>ota_0</code> 的起始地址</td></tr>
</tbody>
</table>
</div>

<p class="muted">最后那句 <code>Hard resetting via RTS pin</code> 很重要——它说明烧写完自动复位了，不需要你手动按 RST。</p>

<h3>卡在 <code>Connecting....</code> 怎么办</h3>
<p>说明没进下载模式。手动进一次：</p>
<ol class="steps">
  <li><span class="st-t">按住 <strong>BOOT</strong> 键不松</span></li>
  <li><span class="st-t">点一下 <strong>RST</strong>（或 EN）键</span></li>
  <li><span class="st-t">松开 BOOT</span><span class="st-d">此时芯片进入下载模式，重新点烧写即可</span></li>
</ol>

<h2>本节验收</h2>
<ul class="checklist">
  <li><label><input type="checkbox" data-ck="f1"><span><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>B</kbd> 编译成功，出现 <code>Project build complete</code></span></label></li>
  <li><label><input type="checkbox" data-ck="f2"><span><code>firmware/build/spider8.bin</code> 存在，大小约 200 KB</span></label></li>
  <li><label><input type="checkbox" data-ck="f3"><span>设备管理器能看到 <code>COM7</code>，且没有黄色感叹号</span></label></li>
  <li><label><input type="checkbox" data-ck="f4"><span>烧写输出 4 行 <code>Hash of data verified.</code></span><span class="hint">少一行说明某个镜像没写进去，重烧</span></label></li>
  <li><label><input type="checkbox" data-ck="f5"><span>看到 <code>Hard resetting via RTS pin</code></span></label></li>
</ul>
`
},

/* ------------------------------------------------------------------ 04 */
{
  id: 'accept',
  num: '04',
  title: '验收',
  subtitle: '读懂串口日志，并确认板子真的是 N16R8',
  html: String.raw`
<p class="lead">烧写成功不等于验收通过。这一节要确认三件事：<strong>日志链路通</strong>、<strong>芯片规格对</strong>、<strong>程序在稳定运行</strong>。最后给了一个自动判定器，把日志粘进去就能核对。</p>

<h2>打开监视</h2>
<p><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> → <code>Run Task</code> → <strong>ESP-IDF: 串口监视 (monitor)</strong> → 串口弹窗直接回车（默认已是 COM7）。</p>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">monitor 是常驻程序，"正在执行中"是正常的</span>
    <p>它不像 build/flash 会跑完退出。退出方式是按 <kbd>Ctrl</kbd>+<kbd>]</kbd>。<strong>不退出它会一直占着 COM7</strong>，此时再点烧写会失败。想看完整开机横幅：monitor 开着时按一下板子上的 RST 键。</p>
  </div>
</div>

<h2>逐行读懂启动日志</h2>
<p>日志格式是固定的四段：<code>级别 (时间戳ms) TAG: 内容</code>。级别里 <code>E</code> 错误、<code>W</code> 警告、<code>I</code> 信息、<code>D</code> 调试。</p>

<div class="code" data-lang="text"><pre><code>I (24) boot: ESP-IDF v5.5.5-dirty 2nd stage bootloader
I (44) boot.esp32s3: SPI Flash Size : 16MB
I (226) esp_psram: Found 8MB PSRAM device
I (676) cpu_start: GPIO 44 and 43 are used as console UART I/O pins
I (676) cpu_start: cpu freq: 240000000 Hz
I (682) app_init: Project name:     spider8
I (686) app_init: App version:      0.1.0
I (782) spider8: ==============================================
I (788) spider8:   Spider8 四足蜘蛛机器人
I (792) spider8:   固件   : spider8 v0.1.0 (build Oct  3 2026 10:00:50)
I (799) spider8:   ESP-IDF: v5.5.5-dirty
I (802) spider8:   芯片   : ESP32-S3 rev2, 2 核
I (807) spider8:   Flash  : 16 MB
I (810) spider8:   PSRAM  : 8192 KB
I (813) spider8:   可用RAM: 419 KB
I (822) spider8:   分区表:
I (825) spider8:   [0] nvs      type=1 subtype=2   addr=0x009000 size=24KB
I (846) spider8:   [3] ota_0    type=0 subtype=16  addr=0x020000 size=3072KB
I (853) spider8:   [4] ota_1    type=0 subtype=17  addr=0x320000 size=3072KB
I (861) spider8:   [5] littlefs type=1 subtype=130 addr=0x620000 size=10048KB
I (868) spider8:   [6] coredump type=1 subtype=3   addr=0xff0000 size=64KB
I (881) spider8: heartbeat #1, free heap=8764823, min-free=8732712
I (1887) spider8: heartbeat #2, free heap=8769271, min-free=8732712</code></pre></div>

<div class="table-wrap">
<table>
<thead><tr><th>关键行</th><th>含义</th><th>合格值</th></tr></thead>
<tbody>
<tr><td><code>SPI Flash Size : 16MB</code></td><td>引导程序识别到的 Flash 容量</td><td><strong>16MB</strong> → N16 规格</td></tr>
<tr><td><code>Found 8MB PSRAM device</code></td><td>PSRAM 型号与容量</td><td><strong>8MB</strong> → R8 规格，且是 Octal 模式</td></tr>
<tr><td><code>cpu freq: 240000000 Hz</code></td><td>CPU 主频</td><td>240 MHz</td></tr>
<tr><td><code>GPIO 44 and 43 are used as console UART</code></td><td>日志从哪两个脚输出</td><td>本项目走原生 USB，这两脚不要挪用</td></tr>
<tr><td><code>Flash : 16 MB</code></td><td>应用自己探测的结果</td><td>必须和引导程序报的一致</td></tr>
<tr><td><code>PSRAM : 8192 KB</code></td><td>应用侧探测的 PSRAM 堆</td><td><strong>8192 KB</strong>；若是 0 说明 <code>CONFIG_SPIRAM</code> 没开</td></tr>
<tr><td><code>heartbeat #n</code></td><td>1 Hz 心跳，说明调度器在跑</td><td>每秒 +1，持续不断</td></tr>
<tr><td><code>free heap / min-free</code></td><td>当前空闲堆 / 历史最低水位</td><td>两个值都应<strong>平稳</strong>，而不是一路下降</td></tr>
</tbody>
</table>
</div>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">为什么"应用自己报"这一层不能省</span>
    <p>引导程序报的 16MB 是"Flash 芯片说自己是多大"，应用报的是"我实际能用的堆有多大"。两者不一致，往往意味着分区表或 PSRAM 模式配错了。横幅里同时报出来，就是为了让矛盾无处可藏。</p>
  </div>
</div>

<div class="callout warn">
  <span class="ico">!</span>
  <div>
    <span class="ttl">看懂 free heap 才算会验收</span>
    <p><code>free heap</code> 是<strong>瞬时值</strong>，会随分配波动；<code>min-free</code> 是<strong>历史最低水位</strong>。判断有没有内存泄漏只看 min-free：它一路往下掉就是漏了，纹丝不动才是健康。本项目实测 min-free 稳定在 8732712 字节不动。</p>
  </div>
</div>

<h2>日志自动判定器</h2>
<p>把 <code>monitor</code> 里的输出整段粘进来，下面会自动核对关键项。全部在本地做字符串匹配，不会上传任何内容。</p>

<div class="tester">
  <textarea id="logInput" spellcheck="false" placeholder="把 idf.py monitor 的输出整段粘贴到这里（含 boot 行与 heartbeat 行）"></textarea>
  <div class="tester-actions">
    <button class="btn" id="logCheckBtn">自动检测</button>
    <button class="btn ghost" id="logSampleBtn">填入示例日志</button>
    <button class="btn ghost" id="logClearBtn">清空</button>
  </div>
  <div class="result" id="logResult"></div>
</div>

<h2>验收清单</h2>
<ul class="checklist">
  <li><label><input type="checkbox" data-ck="a1"><span>串口能出现完整启动横幅，<strong>中文不乱码</strong><span class="hint">乱码是控制台代码页问题，不是固件问题</span></span></label></li>
  <li><label><input type="checkbox" data-ck="a2"><span><code>Flash : 16 MB</code> → N16 规格确认</span></label></li>
  <li><label><input type="checkbox" data-ck="a3"><span><code>PSRAM : 8192 KB</code> → R8 规格确认，且为 Octal</span></label></li>
  <li><label><input type="checkbox" data-ck="a4"><span>分区表列出 <code>ota_0</code> / <code>ota_1</code> / <code>littlefs</code> / <code>coredump</code></span></label></li>
  <li><label><input type="checkbox" data-ck="a5"><span><code>heartbeat</code> 每秒一条，连续 10 条以上不断</span></label></li>
  <li><label><input type="checkbox" data-ck="a6"><span><code>min-free</code> 在 10 条心跳内保持不变</span><span class="hint">变化超过几十 KB 就要查是不是有东西在漏</span></label></li>
  <li><label><input type="checkbox" data-ck="a7"><span>CPU 240 MHz、双核</span></label></li>
</ul>

<h2>三种典型失败形态</h2>
<div class="table-wrap">
<table>
<thead><tr><th>现象</th><th>最可能的原因</th><th>去哪查</th></tr></thead>
<tbody>
<tr><td>监视窗口一片空白，什么都没有</td><td>串口选错 / 板子没跑起来 / 波特率不是 115200</td><td><a href="#troubleshoot">06 章 · 无输出</a></td></tr>
<tr><td>中文变成 <code>??????</code> 或乱码</td><td>控制台代码页 936，固件是 UTF-8</td><td><a href="#troubleshoot">06 章 · 乱码</a></td></tr>
<tr><td>横幅出来几秒后就重启（反复看到 bootloader 行）</td><td>看门狗复位 / 栈溢出 / 断言失败</td><td><a href="#troubleshoot">06 章 · 反复重启</a></td></tr>
</tbody>
</table>
</div>
`
},

/* ------------------------------------------------------------------ 05 */
{
  id: 'commands',
  num: '05',
  title: '命令详解',
  subtitle: '每条命令在做什么，参数为什么要这么填',
  html: String.raw`
<p class="lead">这一节是速查表。不用背——出问题时回来看一眼，比翻搜索引擎快。</p>

<h2>idf.py 子命令</h2>
<div class="table-wrap">
<table>
<thead><tr><th>子命令</th><th>作用</th><th>耗时</th></tr></thead>
<tbody>
<tr><td><code>build</code></td><td>增量编译。只重编改动过的文件及其依赖</td><td>首次 1~5 分钟，之后约 20 秒</td></tr>
<tr><td><code>flash</code></td><td>烧写 4 个镜像到 Flash</td><td>10~30 秒</td></tr>
<tr><td><code>monitor</code></td><td>打开串口监视，实时看日志</td><td>常驻，<kbd>Ctrl</kbd>+<kbd>]</kbd> 退出</td></tr>
<tr><td><code>flash monitor</code></td><td>先烧写再监视，一条命令走完</td><td>—</td></tr>
<tr><td><code>menuconfig</code></td><td>全屏菜单配置（Kconfig），改完写进 <code>sdkconfig</code></td><td>交互式</td></tr>
<tr><td><code>set-target esp32s3</code></td><td>切换目标芯片。<strong>会清空 build 目录</strong></td><td>之后需全量重编</td></tr>
<tr><td><code>fullclean</code></td><td>删掉整个 <code>build/</code>，不动源码</td><td>秒级</td></tr>
<tr><td><code>clean</code></td><td>只删编译产物，保留 CMake 缓存</td><td>秒级</td></tr>
<tr><td><code>reconfigure</code></td><td>重跑 CMake。改了 <code>CMakeLists.txt</code> 但没生效时用</td><td>数秒</td></tr>
<tr><td><code>size</code></td><td>查看固件各段占用（text/data/bss）</td><td>秒级</td></tr>
<tr><td><code>size-components</code></td><td>按组件列占用，找出谁把 Flash 吃掉了</td><td>秒级</td></tr>
<tr><td><code>coredump-info</code></td><td>解析崩溃现场：任务名、PC、调用栈</td><td>秒级</td></tr>
<tr><td><code>erase-flash</code></td><td><span class="tag red">危险</span> 整片擦除，包括 NVS 和校准数据</td><td>十几秒</td></tr>
</tbody>
</table>
</div>

<h3>常用附加参数</h3>
<div class="table-wrap">
<table>
<thead><tr><th>参数</th><th>含义</th><th>本项目取值</th></tr></thead>
<tbody>
<tr><td><code>-p</code> / <code>--port</code></td><td>指定串口</td><td><code>COM7</code>（必填，不填会尝试自动探测）</td></tr>
<tr><td><code>-b</code> / <code>--baud</code></td><td>烧写波特率</td><td>默认 460800。线材差或长时可降到 115200</td></tr>
<tr><td><code>-v</code> / <code>--verbose</code></td><td>输出详细构建日志</td><td>排查 CMake 问题时加</td></tr>
</tbody>
</table>
</div>

<div class="callout danger">
  <span class="ico">×</span>
  <div>
    <span class="ttl"><code>erase-flash</code> 会清掉什么</span>
    <p>整片 Flash，包括 NVS 里的 WiFi 凭据和<strong>舵机校准值</strong>。M0-2 之后，校准数据丢了就得重新逐关节标定一遍。用之前先想清楚，或者先备份 NVS。</p>
  </div>
</div>

<h2>项目包装脚本</h2>
<p><code>tools/idf.ps1</code> 是日常真正在用的入口。它比裸 <code>idf.py</code> 多做四件事：定位 IDF 路径、硬绑定 venv、激活环境、monitor 时切 UTF-8。</p>

<div class="code" data-lang="powershell"><pre><code># 基本用法：把 idf.py 的任意子命令透传进去
..\tools\idf.ps1 build
..\tools\idf.ps1 menuconfig

# 带串口
..\tools\idf.ps1 -Port COM7 flash
..\tools\idf.ps1 -Port COM7 monitor
..\tools\idf.ps1 -Port COM7 flash monitor

# 不填子命令 = 显示 idf.py 的帮助
..\tools\idf.ps1</code></pre></div>

<div class="table-wrap">
<table>
<thead><tr><th>参数</th><th>说明</th></tr></thead>
<tbody>
<tr><td><code>-Port</code></td><td>串口号。不填则把决定权交给 idf.py / esptool 自动探测（多串口机器上不推荐）</td></tr>
<tr><td><code>剩余参数</code></td><td>原样透传给 <code>idf.py</code>，所以 idf.py 新增子命令不用改脚本</td></tr>
</tbody>
</table>
</div>

<h2>为什么它要"硬绑定 venv"</h2>
<p>这是本机最隐蔽的一个坑，值得单独讲清楚。ESP-IDF 激活时会执行 <code>tools/activate.py</code>，而它<strong>用「当前 python 的版本号」去拼虚拟环境目录名</strong>：</p>

<div class="code" data-lang="text"><pre><code>idf.ps1
 └─ . E:\esp\esp-idf\export.ps1
      └─ python tools/activate.py --export        ← 用的是 PATH 里的 python
           └─ idf_tools.get_python_env_path()
                venv 路径 = IDF_PYTHON_ENV_PATH
                          ?? &lt;ToolsPath&gt;/python_env/idf&lt;IDF版本&gt;_py&lt;当前python主.次&gt;_env
                                                                        ↑ 跟着当前 python 走</code></pre></div>

<p>本机 PATH 首选 python 是 3.14.6，于是它去拼 <code>idf5.5_py3.14_env</code>——但实际装的是 <code>idf5.5_py3.13_env</code>，于是 <code>not found</code>。</p>

<p><code>idf.ps1</code> 的修法是双保险：</p>
<ol>
  <li><strong>硬绑定</strong>：扫 <code>E:\esp\.espressif\python_env\idf*_env</code>，把找到的目录直接赋给 <code>IDF_PYTHON_ENV_PATH</code>（<code>idf_tools.py</code> 对该变量最高优先级，不再看 python 版本）</li>
  <li><strong>PATH 前置</strong>：把 venv 的 <code>python.exe</code> 目录插到 PATH 最前，保证后续依赖检查也跑在 venv 里</li>
  <li>找不到就<strong>明确报错退出</strong>，不静默退化成系统 python（那样后面会冒一堆玄学错误）</li>
</ol>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">万能排查命令</span>
    <p>怀疑 venv 有问题时，直接用它跑一次激活：</p>
    <div class="code" data-lang="powershell"><pre><code>E:\esp\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe &#96;
  E:\esp\esp-idf\tools\activate.py --export</code></pre></div>
    <p>能跑通说明 venv 本身没问题，问题在"谁来调它"。</p>
  </div>
</div>

<h2>诊断命令速查</h2>
<p>这几条和 ESP-IDF 无关，但排错时天天用。</p>

<div class="code" data-lang="powershell"><pre><code># 当前有哪些串口
[System.IO.Ports.SerialPort]::GetPortNames()

# 串口设备的完整名字与状态（判断驱动是否正常）
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.Name -match '\(COM\d+\)' } |
  Select-Object Name, Status

# 谁在占着串口（有 python / esptool 进程就是它在用）
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -match 'python|esptool' } |
  Select-Object ProcessId, Name

# 当前控制台代码页（936 = GBK，看中文日志会乱码）
chcp

# PATH 里所有同名的 python（排查为什么版本不对）
Get-Command python -All | Select-Object Source

# ESP-IDF 子模块状态（"-" 开头表示未初始化）
git -C E:\esp\esp-idf submodule status</code></pre></div>

<h2>任务 ↔ 命令对照</h2>
<div class="table-wrap">
<table>
<thead><tr><th>VS Code 任务</th><th>实际执行</th></tr></thead>
<tbody>
<tr><td>ESP-IDF: 编译 (build)</td><td><code>powershell -NoProfile -File tools\idf.ps1 build</code>（cwd = firmware）</td></tr>
<tr><td>ESP-IDF: 烧写 (flash)</td><td><code>... -File tools\idf.ps1 -Port COM7 flash</code></td></tr>
<tr><td>ESP-IDF: 串口监视 (monitor)</td><td><code>... -File tools\idf.ps1 -Port COM7 monitor</code></td></tr>
<tr><td>ESP-IDF: 编译+烧写+监视</td><td><code>... -File tools\idf.ps1 -Port COM7 flash monitor</code></td></tr>
<tr><td>ESP-IDF: 设置目标芯片</td><td><code>... -File tools\idf.ps1 set-target esp32s3</code></td></tr>
<tr><td>ESP-IDF: 菜单配置</td><td><code>... -File tools\idf.ps1 menuconfig</code></td></tr>
<tr><td>ESP-IDF: 全量清理</td><td><code>... -File tools\idf.ps1 fullclean</code></td></tr>
</tbody>
</table>
</div>

<p>想在终端里直接复现某个任务，把第二列整条粘进去即可——这就是"任务出问题时怎么手敲验证"的办法。</p>
`
}

,

/* ------------------------------------------------------------------ 06 */
{
  id: 'troubleshoot',
  num: '06',
  title: '错误处理',
  subtitle: '9 个真实故障：现象、原文、根因、验证',
  html: String.raw`
<p class="lead">这一章的每一条都来自真实记录，报错原文是直接从终端里抓的。先看通用排错顺序，再按关键字搜索。</p>

<h2>通用排错五步</h2>
<div class="table-wrap">
<table>
<thead><tr><th>顺序</th><th>问自己</th><th>怎么查</th></tr></thead>
<tbody>
<tr><td>1</td><td>环境激活了吗？</td><td><code>..\tools\idf.ps1 --version</code> 能不能输出</td></tr>
<tr><td>2</td><td>改的是谁的配置？</td><td>任务 / 终端 profile / 全局环境变量，三者会互相干扰</td></tr>
<tr><td>3</td><td>是硬件问题还是软件问题？</td><td>设备管理器有没有 COM 口；换根线、换个 USB 口试试</td></tr>
<tr><td>4</td><td>是缓存脏了吗？</td><td><code>fullclean</code> 后重编，能好就是缓存</td></tr>
<tr><td>5</td><td>报错的是真因还是连带伤害？</td><td>往上翻第一条错误，后面的往往只是它引发的雪崩</td></tr>
</tbody>
</table>
</div>

<div class="callout info">
  <span class="ico">i</span>
  <div>
    <span class="ttl">看报错要"往上翻"</span>
    <p>典型例子：<code>管道元素中的"."后面的表达式生成无效的对象</code> 看着像 PowerShell 语法错误，实际只是"venv 没找到 → 变量为空 → 点源空字符串"的连带伤害。<strong>第一条错误才是根因。</strong></p>
  </div>
</div>

<h2>故障速查</h2>

<div class="tools-row">
  <div class="search-box">
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="var(--ink-3)" stroke-width="1.5" stroke-linecap="round"><circle cx="7" cy="7" r="4.3"/><path d="M10.3 10.3L13.5 13.5"/></svg>
    <input id="faultSearch" type="search" placeholder="粘贴报错关键字，例如 py3.14_env / submodule / 乱码">
  </div>
  <button class="chip on" data-cat="all">全部</button>
  <button class="chip" data-cat="env">环境安装</button>
  <button class="chip" data-cat="vscode">VS Code</button>
  <button class="chip" data-cat="flash">烧写与串口</button>
  <button class="chip" data-cat="log">日志</button>
  <button class="chip" data-cat="script">脚本</button>
</div>

<details class="fault" data-cat="vscode">
  <summary>
    <span class="fno">01</span>
    <span class="ftitle">idf5.5_py3.14_env not found，紧跟着 "表达式生成无效的对象"
      <span class="fsum">最常见的一个。看起来是两个错，其实只有一个根因</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>报错原文</dt>
      <dd>
        <div class="code" data-lang="text"><pre><code>ERROR: ESP-IDF Python virtual environment
  "C:\Users\aaron\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe" not found.

管道元素中的"."后面的表达式生成无效的对象。
所在位置 E:\esp\esp-idf\export.ps1:27 字符: 3
+ . $idf_exports</code></pre></div>
      </dd>
      <dt>根因</dt>
      <dd>IDF 的 <code>activate.py</code> 用<strong>当前 python 的版本号</strong>去拼 venv 目录名。PATH 首选的 <code>python</code> 是 3.14.6，于是拼出 <code>py3.14_env</code>，而实际装的是 <code>py3.13_env</code>。venv 找不到 → <code>$idf_exports</code> 为空 → 点源空字符串报"表达式无效"。<strong>后半句是连带伤害，不是语法问题。</strong></dd>
      <dt>修复</dt>
      <dd>
        用项目里的 <code>tools/idf.ps1</code>（它硬绑定 <code>IDF_PYTHON_ENV_PATH</code>）。若是在 VS Code 里，确认任务已带 <code>options.shell</code>。
        <div class="code" data-lang="powershell"><pre><code># 手动验证 venv 本身是否正常
E:\esp\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe &#96;
  E:\esp\esp-idf\tools\activate.py --export

# 确认目录确实存在（名字里的 3.13 要和实际一致）
dir E:\esp\.espressif\python_env</code></pre></div>
      </dd>
      <dt>验证</dt>
      <dd><code>..\tools\idf.ps1 --version</code> 输出 <code>ESP-IDF v5.5.5</code>，且开头能看到 <code>venv : ...\idf5.5_py3.13_env</code>。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="vscode">
  <summary>
    <span class="fno">02</span>
    <span class="ftitle">任务一直显示"正在执行中"，永远不结束
      <span class="fsum">带 -NoExit 的终端 profile 不能用来跑任务</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>现象</dt>
      <dd>点烧写后终端停住，任务栏一直转，无法结束；同时可能附带 01 号故障的报错。</dd>
      <dt>根因</dt>
      <dd>默认终端 profile 带了 <code>-NoExit</code> 和一个 <code>-Command</code>。VS Code 跑 shell 任务时会<strong>把任务命令行追加到 profile 参数后面</strong>，于是整条任务被 <code>-Command</code> 吞成 <code>export.ps1</code> 的参数。真实进程是这样的：
        <div class="code" data-lang="text"><pre><code>powershell.exe -NoExit -Command . 'E:\esp\esp-idf\export.ps1'
        -Command powershell -NoProfile -File ...\idf.ps1 -Port COM7 flash
                     ↑ 全被吃掉</code></pre></div>
      </dd>
      <dt>修复</dt>
      <dd>给任务加 <code>options.shell</code>，显式指定干净 shell（本项目 <code>tasks.json</code> 已全部加上）。终端 profile 与任务彻底解耦。</dd>
      <dt>已经卡住怎么清</dt>
      <dd>点终端标签右侧的 🗑，或 <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> → <code>Tasks: Terminate Task</code>，或直接关终端面板。</dd>
      <dt>验证</dt>
      <dd>任务跑完<strong>自行退出</strong>（不再显示"正在执行中"）。本项目实测 build 22.8 秒后退出码 0。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="env">
  <summary>
    <span class="fno">03</span>
    <span class="ftitle">编译报子模块缺失：Unable to find current revision in submodule path
      <span class="fsum">Gitee 镜像不全 + 全局 git 代理改写，两个因素叠加</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>报错原文</dt>
      <dd>
        <div class="code" data-lang="text"><pre><code>CMake Error: Could not find micro-ecc source directory
Unable to find current revision in submodule path 'components/bt/controller/lib_esp32'
fatal: repository '...' not found</code></pre></div>
      </dd>
      <dt>根因</dt>
      <dd>两层原因：① Gitee 上的 ESP-IDF 镜像<strong>缺了若干子模块仓库</strong>（请求会要求输账号密码 = 404）；② 本机 git 全局配置有一条把 <code>github.com</code> 改写到已停服的 <code>ghproxy.com</code> 的规则，导致克隆走坏代理。</dd>
      <dt>修复</dt>
      <dd>
        <div class="code" data-lang="powershell"><pre><code># 1) 先看有没有失效的改写规则
git config --global --get-regexp insteadof

# 2) 临时绕过全局配置（Git Bash 用 GIT_CONFIG_GLOBAL=/dev/null）
cd E:\esp\esp-idf
$env:GIT_CONFIG_GLOBAL = "NUL"
git submodule update --init --recursive --jobs 8

# 3) 仍然报 "Unable to find current revision" 的，先 deinit 再重拉
git submodule deinit -f components/bt/controller/lib_esp32
git submodule update --init --recursive --jobs 8

# 4) 确认清理干净（输出里不该再有 "-" 开头的行）
git submodule status | Select-String '^-'</code></pre></div>
      </dd>
      <dt>验证</dt>
      <dd>重新编译不再报找不到子模块；<code>git submodule status</code> 里每行都以空格或 <code>+</code> 开头，没有 <code>-</code>。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="env">
  <summary>
    <span class="fno">04</span>
    <span class="ftitle">克隆时卡住、要求输账号密码、速度极慢
      <span class="fsum">根因往往不在网速，而在一条过期的 URL 改写规则</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>现象</dt>
      <dd><code>git clone</code> 长时间无响应，或弹出 <code>Username for 'https://github.com':</code>。</dd>
      <dt>根因</dt>
      <dd>全局 <code>insteadOf</code> 规则把 GitHub 地址改写到了一个已停服或不可达的代理域名。要求输密码通常意味着目标仓库返回 404/401（私有镜像缺库也会这样）。</dd>
      <dt>修复</dt>
      <dd>
        <div class="code" data-lang="powershell"><pre><code># 看清楚有哪些改写
git config --global --get-regexp insteadof

# 清掉失效的那条（示例，按实际输出替换）
git config --global --unset url.https://ghproxy.com/https://github.com/.insteadof

# 只想临时绕过、不改全局：
$env:GIT_CONFIG_GLOBAL = "NUL"   # 当前窗口有效
$env:GIT_TERMINAL_PROMPT = "0"   # 禁止交互式要密码，快速失败</code></pre></div>
      </dd>
      <dt>验证</dt>
      <dd>单仓库克隆测试：<code>git clone --depth 1 https://github.com/espressif/esp-coex-lib.git</code>，本项目实测 4.6 MB 用 6.8 秒。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="log">
  <summary>
    <span class="fno">05</span>
    <span class="ftitle">串口日志中文变成 ?????? 或乱码
      <span class="fsum">控制台代码页 936 与固件 UTF-8 不匹配，不是固件坏了</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>现象</dt>
      <dd>日志里中文显示成 <code>??????</code>、<code>鍥句欢</code> 之类的乱码，英文和数字正常。</dd>
      <dt>根因</dt>
      <dd>Windows 控制台默认代码页是 936（GBK），而固件输出的是 UTF-8。另外用 .NET <code>SerialPort</code> 读串口时，<strong>默认编码是 ASCII</strong>，不显式设 UTF-8 会把中文变成问号——这一条连"固件正常"都会被误判。</dd>
      <dt>修复</dt>
      <dd>
        <code>tools/idf.ps1</code> 在检测到 <code>monitor</code> 子命令时会自动执行：
        <div class="code" data-lang="powershell"><pre><code>[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
chcp 65001

# 用 .NET 读串口时必须显式设编码
$sp = New-Object System.IO.Ports.SerialPort('COM7',115200)
$sp.Encoding = [System.Text.Encoding]::UTF8</code></pre></div>
        <p class="muted" style="margin-top:8px">注意：只在 monitor 时切。menuconfig 是全屏 TUI，切代码页反而会显示错乱。</p>
      </dd>
      <dt>怎么判断是不是真乱码</dt>
      <dd>看有没有 <code>I (881) spider8:</code> 这类前缀正常、只有中文异常的情况。前缀正常 = 固件没问题，纯显示层问题。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="script">
  <summary>
    <span class="fno">06</span>
    <span class="ftitle">.ps1 脚本报"意外的标记 }"或字符串未终止
      <span class="fsum">PowerShell 5.1 需要 UTF-8 BOM，否则中文被按 GBK 解码</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>报错原文</dt>
      <dd><div class="code" data-lang="text"><pre><code>所在位置 ...\check_env.ps1:24 字符: 1
+ }
+ ~
意外的标记“}”。
所在位置 ...\check_env.ps1:62 字符: 12
+     } else {
+            ~
字符串缺少终止符: "。</code></pre></div>
      </dd>
      <dt>根因</dt>
      <dd>Windows PowerShell 5.1 读取无 BOM 的 .ps1 时会按本地代码页（GBK）解码。UTF-8 的中文字节被误读后，引号和括号的配对关系崩掉，于是报出一堆"语法错误"——<strong>但脚本本身其实是合法的</strong>。</dd>
      <dt>修复</dt>
      <dd>保存为 <strong>UTF-8 with BOM + CRLF</strong>。用 PowerShell 转一次：
        <div class="code" data-lang="powershell"><pre><code>$p = ".\tools\check_env.ps1"
$s = Get-Content $p -Raw -Encoding UTF8
[System.IO.File]::WriteAllText((Resolve-Path $p), $s, (New-Object System.Text.UTF8Encoding $true))</code></pre></div>
        <p class="muted" style="margin-top:8px">本项目所有 <code>.ps1</code> 都按 BOM + CRLF 保存，并做过 <code>[Parser]::ParseFile</code> 语法校验。</p>
      </dd>
      <dt>验证</dt>
      <dd>读前三个字节应为 <code>EF BB BF</code>：
        <div class="code" data-lang="powershell"><pre><code>[System.IO.File]::ReadAllBytes(".\tools\check_env.ps1")[0..2] | ForEach-Object { $_.ToString("X2") }</code></pre></div>
      </dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="flash">
  <summary>
    <span class="fno">07</span>
    <span class="ftitle">烧写卡在 Connecting....，最后 Failed to connect
      <span class="fsum">没进下载模式，或者串口根本不是你填的那个</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>报错原文</dt>
      <dd><div class="code" data-lang="text"><pre><code>Connecting........_____....._____....._____
A fatal error occurred: Failed to connect to ESP32-S3:
  No serial data received.</code></pre></div>
      </dd>
      <dt>根因</dt>
      <dd>三种可能：① 芯片没进下载模式；② 串口号填错（换了 USB 口就会变）；③ 串口被别的程序占用（上一次 monitor 没退出）。</dd>
      <dt>修复</dt>
      <dd>
        <ol>
          <li>手动进下载模式：<strong>按住 BOOT → 点 RST → 松开 BOOT</strong></li>
          <li>确认串口号：<code>[System.IO.Ports.SerialPort]::GetPortNames()</code></li>
          <li>查占用：<code>Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'python|esptool' }</code></li>
          <li>线材差就降速：<code>..\tools\idf.ps1 -Port COM7 -b 115200 flash</code></li>
        </ol>
      </dd>
      <dt>验证</dt>
      <dd>看到四行 <code>Hash of data verified.</code> 加 <code>Hard resetting via RTS pin</code>。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="log">
  <summary>
    <span class="fno">08</span>
    <span class="ftitle">monitor 打开了却一片空白，或提示串口拒绝访问
      <span class="fsum">要么选错口，要么口被别人占着</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>现象 A：一片空白</dt>
      <dd>程序只在启动时打印横幅，之后只有 1 Hz 心跳——如果打开 monitor 的时机比启动晚，会看到空白几秒。按一下板子 <strong>RST</strong> 重放完整启动日志。</dd>
      <dt>现象 B：拒绝访问</dt>
      <dd>
        <div class="code" data-lang="text"><pre><code>could not open port 'COM7': PermissionError(13, '拒绝访问。')</code></pre></div>
        说明有别的进程占着串口。最常见的是上一次 monitor 没退出（<kbd>Ctrl</kbd>+<kbd>]</kbd>），其次是串口助手之类的工具。
        <div class="code" data-lang="powershell"><pre><code># 找出占用者
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -match 'python|esptool|putty|SecureCRT' } |
  Select-Object ProcessId, Name

# 确认口空闲（能打开就说明没人占）
$sp = New-Object System.IO.Ports.SerialPort('COM7',115200)
try { $sp.Open(); "COM7 空闲"; $sp.Close() } catch { "被占用: $($_.Exception.Message)" }</code></pre></div>
      </dd>
      <dt>记住</dt>
      <dd><strong>同一时刻只能有一个程序用同一个串口。</strong>monitor 和 flash 不能同时跑。</dd>
    </dl>
  </div>
</details>

<details class="fault" data-cat="log">
  <summary>
    <span class="fno">09</span>
    <span class="ftitle">板子反复重启，bootloader 日志一直重复刷
      <span class="fsum">看门狗复位 / 栈溢出 / 断言失败，三种要分开看</span>
    </span>
  </summary>
  <div class="fbody">
    <dl>
      <dt>怎么区分</dt>
      <dd>
        <div class="table-wrap">
          <table>
          <thead><tr><th>日志关键字</th><th>含义</th><th>常见原因</th></tr></thead>
          <tbody>
          <tr><td><code>Task watchdog got triggered</code></td><td>某个任务长时间不让出 CPU</td><td>死循环里漏了 <code>vTaskDelay</code></td></tr>
          <tr><td><code>***ERROR*** A stack overflow in task</code></td><td>任务栈不够</td><td>栈单位是"字"不是字节，或局部大数组</td></tr>
          <tr><td><code>Guru Meditation Error</code></td><td>非法访问 / 空指针 / 对齐错误</td><td>野指针、越界、静态任务返回</td></tr>
          <tr><td><code>assert failed</code></td><td>断言失败</td><td><code>ESP_ERROR_CHECK</code> 捕获到的错误</td></tr>
          </tbody>
          </table>
        </div>
      </dd>
      <dt>根因（最常见的一种）</dt>
      <dd>FreeRTOS 任务函数<strong>不允许 return</strong>，必须 <code>for(;;)</code> 包住；循环体里必须有 <code>vTaskDelay</code>，否则会饿死空闲任务触发看门狗。</dd>
      <dt>深挖工具</dt>
      <dd>
        本项目分区表里留了 64 KB 的 <code>coredump</code> 分区，崩溃现场会自动存进去：
        <div class="code" data-lang="powershell"><pre><code>..\tools\idf.ps1 coredump-info</code></pre></div>
        能还原出崩溃时的任务名、PC 指针和调用栈——这比看串口那几行 <code>Backtrace:</code> 地址有用得多。
      </dd>
    </dl>
  </div>
</details>

<div class="no-result" id="noFault" style="display:none">没有匹配的条目。试试更短的关键字，比如 <code>venv</code>、<code>submodule</code>、<code>乱码</code>。</div>
`
},

/* ------------------------------------------------------------------ 07 */
{
  id: 'appendix',
  num: '07',
  title: '附录：main.c 逐段讲解',
  subtitle: '为什么一个"什么都没干"的文件值得认真写',
  html: String.raw`
<p class="lead">159 行，跑起来只打印一段横幅和每秒一条心跳。但每一段都对应一个明确的设计决策，它把后面所有代码的<strong>风格、边界和纪律</strong>都定下来了。</p>

<h2>它的职责不是"干活"，是"自证健康"</h2>
<p>启动横幅要证明五件事都通了：编译链路、烧写链路、串口链路、芯片规格、FreeRTOS 调度。真正的运动控制代码（M0-2 起）会另建文件，<code>main.c</code> 一直保持"薄"。</p>

<h2>逐段拆解</h2>

<h3>文件头注释（1–17 行）</h3>
<p>不是装饰。它钉住三件工程约定：本文件属于哪个里程碑、<strong>验收标准写进代码</strong>、目标硬件型号。验收标准只有一份来源，就不会出现"文档说 A、代码做 B"。</p>

<h3>TAG 常量（34 行）</h3>
<div class="code" data-lang="c"><pre><code>static const char *TAG = "spider8";</code></pre></div>
<p>每条日志前面的模块名就是它。ESP32 是<strong>双核 + 多任务</strong>，日志交错输出，没有 TAG 就是一锅粥。<code>static const</code> 让它进只读段，不占 RAM。</p>

<h3>配置宏（40–44 行）</h3>
<div class="code" data-lang="c"><pre><code>#define HEARTBEAT_GPIO        GPIO_NUM_4
#define HEARTBEAT_STACK_SIZE  2048   /* 字，不是字节：2048*4 = 8KB 栈 */
#define HEARTBEAT_PRIORITY    2      /* 低优先级，让位给后续的运动任务 */</code></pre></div>
<p>三个要点：</p>
<ul>
  <li>用宏而不是散落的字面量——8 路舵机的引脚会反复调整，改一处全局生效。</li>
  <li><strong>栈单位是"字"不是字节</strong>。FreeRTOS 的栈参数类型是 <code>StackType_t</code>（4 字节），所以 2048 实际是 8 KB。以为传字节就会开成 1/4 栈然后莫名溢出。</li>
  <li>优先级 2 是故意选低的，给将来的步态引擎和保护任务留位置。</li>
</ul>

<h3>静态分配的两个全局变量（46–48 行）</h3>
<div class="code" data-lang="c"><pre><code>static StaticTask_t s_heartbeat_tcb;
static StackType_t  s_heartbeat_stack[HEARTBEAT_STACK_SIZE];</code></pre></div>
<p><strong><code>static</code> 一个都不能少。</strong>不加的话它们就是 <code>app_main</code> 的局部变量，活在 main 任务的栈上；<code>app_main</code> 一返回栈就回收，心跳任务会接着踩垃圾内存——表现为"跑几秒后随机崩溃"。</p>
<p>用静态分配而非动态的理由：机器人要连续跑几小时，静态分配把"不确定"变成"确定"——地址编译期固定、失败可判断、堆水位平直、崩溃可复现。</p>

<h3>log_partitions()（54–67 行）</h3>
<p>用迭代器查分区表，而不是硬编码打印。因为分区表是<strong>数据</strong>（在 <code>partitions.csv</code>），代码是<strong>逻辑</strong>；硬编码就会出现"改了 CSV 但日志还报旧值"的谎言式日志。</p>
<div class="callout warn">
  <span class="ico">!</span>
  <div>
    <span class="ttl">两个容易埋雷的细节</span>
    <p><code>esp_partition_find</code> 内部会加锁，必须 <code>release</code>。忘了它是<strong>延迟死锁</strong>——下次再调分区 API 才卡住，极难排查。循环结束时 <code>it</code> 已经是 NULL，把 NULL 传给 release 是<strong>合法且必需</strong>的收尾，不是笔误。</p>
    <p>另外 <code>%lu</code> 配 <code>(unsigned long)</code> 强转是保命的：在 IDF 5.x 上格式不匹配会直接崩，不报错只重启。</p>
  </div>
</div>

<h3>print_banner()（69–106 行）</h3>
<p>数据来自两个完全不同的地方：</p>
<div class="table-wrap">
<table>
<thead><tr><th>来源</th><th>函数</th><th>性质</th></tr></thead>
<tbody>
<tr><td>固件头部</td><td><code>esp_app_get_description()</code></td><td><strong>编译期固化</strong>：项目名、版本、编译日期</td></tr>
<tr><td>芯片与内存</td><td><code>esp_chip_info()</code> / <code>esp_flash_get_size()</code> / <code>heap_caps_get_total_size()</code></td><td><strong>运行期真实探测</strong></td></tr>
</tbody>
</table>
</div>
<p>为什么不直接打印 "Flash 16MB" 这个字面量？因为那样你永远不会知道板子对不对。万一卖家发的是 N8R2，硬编码的日志会骗你到 M5 才发现。</p>
<p>最后两个 <code>ESP_LOGW</code> 是灵魂——<strong>自动断言</strong>，把"我以为的硬件"变成"机器验证过的硬件"：</p>
<div class="code" data-lang="c"><pre><code>if (psram_kb &lt; 8192) {
    ESP_LOGW(TAG, "PSRAM 只有 %luKB，预期 8192KB —— N16R8 应为 Octal 8MB，"
                  "请确认 sdkconfig 已启用 CONFIG_SPIRAM_MODE_OCT", (unsigned long)psram_kb);
}</code></pre></div>
<p>还有个知识点：<code>MALLOC_CAP_INTERNAL</code> 与 <code>MALLOC_CAP_SPIRAM</code> 是<strong>两块物理内存</strong>。内部 RAM 约 512 KB 但快，PSRAM 8 MB 但走 SPI 慢。这个区分对我们是刚需——舵机 PWM 的定时器缓冲区和中断处理必须在内部 RAM，放 PSRAM 会因访问延迟导致抖动。</p>

<h3>heartbeat_task()（112–126 行）</h3>
<div class="code" data-lang="c"><pre><code>for (;;) {
    level = !level;
    ESP_ERROR_CHECK(gpio_set_level(HEARTBEAT_GPIO, level));
    ESP_LOGI(TAG, "heartbeat #%lu, free heap=%lu, min-free=%lu", ...);
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
}</code></pre></div>
<p>四个设计点，每个都是硬性要求：</p>
<ol>
  <li><strong>必须 <code>for(;;)</code></strong>。FreeRTOS 任务函数绝不允许返回。一旦 return 会触发删除自身；在静态创建的任务上这个行为是<strong>未定义</strong>的。</li>
  <li><strong>必须有 <code>vTaskDelay</code></strong>。没有它就是不让出 CPU 的死循环，会饿死所有 ≤2 优先级的任务（含空闲任务），几秒内看门狗复位。</li>
  <li><strong>为什么打印 <code>min-free</code></strong>。<code>free heap</code> 是瞬时值，<code>min-free</code> 是历史最低水位——<strong>内存泄漏只有看 min-free 才会露馅</strong>。</li>
  <li><strong>为什么还要翻转 GPIO</strong>。日志依赖串口，串口可能被拔、被占用；LED 是独立于软件的物理证据，双通道验证。而且故意选 GPIO4 = 舵机 CH0 脚位：M0 用 LED 验，M0-2 直接插舵机，<strong>一行代码都不用改</strong>。</li>
</ol>

<h3>app_main()（132–158 行）</h3>
<p><strong><code>app_main</code> 不是 <code>main</code>。</strong>启动链是：ROM → 二级引导 → 创建 <code>main</code> 任务 → 在里面调 <code>app_main()</code> → 返回后 main 任务自动删除自己。所以它是"一次性启动脚本"，末尾不需要死循环。</p>
<div class="callout warn">
  <span class="ico">!</span>
  <div>
    <span class="ttl">别在 app_main 里写长耗时循环</span>
    <p>main 任务默认只有 3584 字节栈，很容易溢出。要常驻的逻辑一律放进独立任务。</p>
  </div>
</div>
<p>几个细节：</p>
<ul>
  <li><code>gpio_config_t</code> 用指定初始化器（<code>.pin_bit_mask = ...</code>），因为结构体字段顺序在 IDF 各版本间会变。</li>
  <li><code>1ULL &lt;&lt; HEARTBEAT_GPIO</code> 必须是 <code>1ULL</code>：<code>pin_bit_mask</code> 是 64 位，用 <code>1 &lt;&lt;</code> 会因符号扩展配错引脚，而且不报错。</li>
  <li>显式关掉 <code>pull_up</code> / <code>pull_down</code>：部分引脚默认带内部上下拉，不关掉接舵机时会有微弱漏电流，导致"嗡嗡"响或轻微抖动。</li>
  <li><code>ESP_ERROR_CHECK</code> 敢用 <code>abort()</code>，是因为 M0 阶段连 GPIO 配置都失败说明硬件/配置彻底不对，继续跑只会产生更难懂的次生错误——快速失败。</li>
</ul>

<div class="callout danger">
  <span class="ico">×</span>
  <div>
    <span class="ttl">这条边界 M0-2 必须改</span>
    <p><strong>舵机控制调用绝不能裸用 <code>ESP_ERROR_CHECK</code>。</strong>一条腿控制失败不该让整机重启，那必须用返回码 + 降级处理。启动阶段可以 fail fast，运行阶段必须容错。</p>
  </div>
</div>

<h2>这个文件确立的三条项目规范</h2>
<div class="cards">
  <div class="card"><span class="k">规范 1</span><h4>运行期禁用动态内存</h4><p>任务栈、队列、缓冲区一律静态分配，堆只读不写。</p></div>
  <div class="card"><span class="k">规范 2</span><h4>任务 = for(;;) + vTaskDelay</h4><p>绝不 return、绝不空转。栈大小事后用 <code>uxTaskGetStackHighWaterMark</code> 实测。</p></div>
  <div class="card"><span class="k">规范 3</span><h4>关键资源上电自检</h4><p>探测真实值并打 WARN，让机器说出真相，而不是让日志说谎。</p></div>
</div>

<h2>下一步：M0-2</h2>
<p>8 路 LEDC PWM（50 Hz，脉宽 500~2500 µs，14 bit 分辨率）+ 角度到脉宽的线性映射 + 每关节 <code>min/max/center/trim</code> 校准 + 斜率限流保护（MG90S 无反馈，靠限制突变速率防瞬间大电流）+ 校准参数落 NVS。</p>
<p>会新增 <code>main/servo.c</code> / <code>servo.h</code> / <code>config.h</code>，并把上面那条「返回码 + 降级」的边界真正落地。</p>

<div class="callout ok">
  <span class="ico">✓</span>
  <div>
    <span class="ttl">舵机没到也能验收</span>
    <p>LED + 220Ω 电阻接信号脚，50 Hz PWM 下亮度随脉宽变化——肉眼就能确认 8 路输出正确。</p>
  </div>
</div>
`
}

];

/* 内容结束 —— 共 8 章：00 开始 / 01 环境 / 02 VS Code / 03 编译烧写 / 04 验收 / 05 命令 / 06 错误 / 07 附录 */
