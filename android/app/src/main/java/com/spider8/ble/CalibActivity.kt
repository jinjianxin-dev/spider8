package com.spider8.ble

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.spider8.ble.databinding.ActivityCalibBinding
import java.util.Locale

/**
 * 舵机 trim / dir 标定页（对应 tools/servo_calib.html）。
 *
 * 关键点：蓝牙回话是「严格单包」，安卓 MTU 256 时净荷 253B，而固件 status
 * 的 8 路表格近千字节 —— 所以这里不用 status 读表，改用新加的
 * `trimget <0-7>`（回显约 13B，任何 MTU 都塞得下）逐路读取。
 *
 * 安全逻辑与网页一致：翻转 dir / 全清之前先 pwm off → cal off → pose stand
 * → 等姿态收敛，避免非站姿下把腿镜像过去猛转。
 */
class CalibActivity : AppCompatActivity(), BleConnection.Listener {

    private lateinit var binding: ActivityCalibBinding
    private lateinit var ble: BleConnection
    private val ui = Handler(Looper.getMainLooper())

    private val trim = FloatArray(8)
    private val dir = IntArray(8) { 1 }
    private var cur = 0
    private var connected = false

    private val names = arrayOf(
        "左前·髋", "左前·膝", "右前·髋", "右前·膝",
        "左后·髋", "左后·膝", "右后·髋", "右后·膝"
    )
    /** 装配基准关节角（trim 零点）：髋 19° / 膝 −38° */
    private val ref = floatArrayOf(19f, -38f, 19f, -38f, 19f, -38f, 19f, -38f)
    /** 2026-11-13 起默认站姿比装配基准多折 10°：髋 +5° / 膝 −10° */
    private val standOff = floatArrayOf(5f, -10f, 5f, -10f, 5f, -10f, 5f, -10f)

    private lateinit var chans: Array<TextView>

    /* ---- 拖滑块节流：最多约 120ms 一条，且保证最后一次一定发出去 ---- */
    private var lastTrimSendMs = 0L
    private var pendingTrimCmd: String? = null
    private val trimTail = Runnable {
        pendingTrimCmd?.let {
            pendingTrimCmd = null
            lastTrimSendMs = SystemClock.elapsedRealtime()
            BleBus.send(it)
        }
    }

    private var dirBusy = false
    private var wiggleBusy = false
    private var suppressSeek = false

    companion object {
        private const val TRIM_LIMIT = 50f
        private val RE_GET = Regex("""T([0-7])=([-+]?[0-9]*\.?[0-9]+)/([-+]?\d+)""")
        private val RE_TRIM = Regex("""CH([0-7])\s+trim\s*=\s*([-+]?[0-9]*\.?[0-9]+)""")
        private val RE_DIR = Regex("""CH([0-7])\s+dir\s*=\s*([-+]?\d+)""")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityCalibBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ble = BleBus.get(this)
        chans = arrayOf(
            binding.chan0, binding.chan1, binding.chan2, binding.chan3,
            binding.chan4, binding.chan5, binding.chan6, binding.chan7
        )

        setupUi()
        renderAll()
        updateConnUi(BleBus.isReady())
    }

    override fun onStart() {
        super.onStart()
        ble.addListener(this)
        updateConnUi(BleBus.isReady())
    }

    override fun onStop() {
        ble.removeListener(this)
        ui.removeCallbacks(trimTail)
        super.onStop()
    }

    private fun setupUi() {
        binding.btnBack.setOnClickListener { finish() }

        chans.forEachIndexed { j, v ->
            v.setOnClickListener { selectChan(j) }
        }

        binding.seekTrim.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                if (suppressSeek) return
                setTrim(progressToValue(p), fromUser)
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {
                flushPendingTrim()   // 保证松手时的最后一拍一定发出去
            }
        })

        binding.btnM1.setOnClickListener { adjust(-1f) }
        binding.btnM05.setOnClickListener { adjust(-0.5f) }
        binding.btnP05.setOnClickListener { adjust(0.5f) }
        binding.btnP1.setOnClickListener { adjust(1f) }
        binding.btnZero.setOnClickListener { setTrim(0f, true) }

        binding.btnDir.setOnClickListener { safeFlipDir() }
        binding.btnReadAll.setOnClickListener { readAll() }
        binding.btnSave.setOnClickListener {
            flushPendingTrim()
            BleBus.send("save")
            logSys("已发 save。看到「标定值已写入 NVS」才算成功。")
        }
        binding.btnStand.setOnClickListener {
            BleBus.send("pose stand")
            logSys("已回站姿（本来就在站姿时舵机不会动，这是正常的）。")
        }
        binding.btnWiggle.setOnClickListener { wiggle() }
        binding.btnPwmOff.setOnClickListener {
            BleBus.send("pwm off")
            logSys("已退出 raw 模式，步态恢复，trim 滑块现在会生效。")
        }
        binding.btnZeroAll.setOnClickListener { zeroAll() }
        binding.btnClearAll.setOnClickListener { clearAllWithConfirm() }

        val quick: Map<View, String> = mapOf(
            binding.btnCStatus to "status",
            binding.btnCPca to "pcadiag",
            binding.btnCPwmOff to "pwm off",
            binding.btnCCalOff to "cal off"
        )
        quick.forEach { (v, cmd) -> v.setOnClickListener { BleBus.send(cmd) } }
        binding.btnCalSend.setOnClickListener { sendTyped() }
        binding.etCalCmd.setOnEditorActionListener { _, _, _ -> sendTyped(); true }
        binding.btnCalClear.setOnClickListener { binding.tvCalLog.text = "" }
    }

    private fun sendTyped() {
        val cmd = binding.etCalCmd.text.toString().trim()
        if (cmd.isEmpty()) return
        BleBus.send(cmd)
        binding.etCalCmd.text?.clear()
    }

    /* ---------------- 数值 / 渲染 ---------------- */

    private fun progressToValue(p: Int): Float = p / 2f - 50f
    private fun valueToProgress(v: Float): Int = ((v + 50f) * 2f).toInt().coerceIn(0, 200)
    private fun fmt(v: Float): String = String.format(Locale.US, "%+.1f", v)
    private fun num(v: Float): String = String.format(Locale.US, "%.1f", v)

    private fun setTrim(v: Float, send: Boolean) {
        val q = (Math.round(v * 2f) / 2f).coerceIn(-TRIM_LIMIT, TRIM_LIMIT)
        trim[cur] = q
        renderAll()
        if (send) sendTrimThrottled(q)
    }

    private fun adjust(delta: Float) = setTrim(trim[cur] + delta, true)

    /** 拖动时节流；命令在构造时绑定当前通道，不会串到刚选中的别的舵机 */
    private fun sendTrimThrottled(v: Float) {
        val cmd = "trim $cur ${num(v)}"
        val now = SystemClock.elapsedRealtime()
        if (now - lastTrimSendMs >= 120) {
            lastTrimSendMs = now
            BleBus.send(cmd)
        } else {
            pendingTrimCmd = cmd
            ui.removeCallbacks(trimTail)
            ui.postDelayed(trimTail, 130)
        }
    }

    private fun flushPendingTrim() {
        ui.removeCallbacks(trimTail)
        pendingTrimCmd?.let {
            pendingTrimCmd = null
            lastTrimSendMs = SystemClock.elapsedRealtime()
            BleBus.send(it)
        }
    }

    private fun selectChan(j: Int) {
        if (j != cur) flushPendingTrim()
        cur = j
        logSys("选中 CH$j ${names[j]}。先点「通电自检」确认它会动，再拖滑块。")
        renderAll()
    }

    private fun renderAll() {
        renderChans()
        renderTable()
        renderSlider()
    }

    private fun renderChans() {
        chans.forEachIndexed { j, v ->
            v.text = "CH$j\n${names[j]}\n${fmt(trim[j])}° / ${if (dir[j] > 0) "+1" else "−1"}"
            v.background = ContextCompat.getDrawable(
                this,
                if (j == cur) R.drawable.bg_chan_sel else R.drawable.bg_chan
            )
        }
    }

    private fun renderTable() {
        val sb = StringBuilder("CH   关节        trim      dir\n")
        for (j in 0..7) {
            sb.append(String.format(Locale.US, "%-5d", j))
                .append(String.format(Locale.US, "%-12s", names[j]))
                .append(String.format(Locale.US, "%-10s", fmt(trim[j]) + "°"))
                .append(if (dir[j] > 0) "+1" else "−1")
                .append(if (j == cur) "  ←" else "")
                .append('\n')
        }
        binding.tvTable.text = sb.toString().trimEnd()
    }

    private fun renderSlider() {
        val v = trim[cur]
        binding.tvBigVal.text = "${fmt(v)}°"
        suppressSeek = true
        binding.seekTrim.progress = valueToProgress(v)
        suppressSeek = false

        val sv = 90f + standOff[cur] * dir[cur] + v
        val warn = if (sv < 0f || sv > 180f) "  ⚠ 超出 0~180° 硬限位" else ""
        binding.tvReadout.text =
            "当前 CH$cur ${names[cur]}（装配基准 ${num(ref[cur])}°，站姿偏移 ${fmt(standOff[cur])}°）\n" +
            "站姿舵机角 = 90° + 偏移×dir + trim = ${num(sv)}°$warn\n" +
            "发出：trim $cur ${num(v)}　方向：dir $cur ${dir[cur]}"

        binding.btnDir.text = if (dirBusy) "🔁 翻转中…先让全机回站姿"
            else "🔁 方向 dir：${if (dir[cur] > 0) "+1" else "−1"}（点一下安全翻转）"
    }

    /* ---------------- 命令动作 ---------------- */

    private fun readAll() {
        flushPendingTrim()
        logSys("读取 8 路 trim/dir（trimget 0~7，逐条回话）…")
        for (j in 0..7) BleBus.send("trimget $j")
        ui.postDelayed({ BleBus.send("trimget $cur") }, 1200)  // 兜底补一次，防止偶发丢包
    }

    private fun safeFlipDir() {
        if (!connected) { toast("先连上 Spider8"); return }
        if (dirBusy || wiggleBusy) { toast("正在忙，等它结束"); return }
        dirBusy = true
        val j = cur
        val next = if (dir[j] > 0) -1 else 1
        logSys("安全翻转 CH$j：pwm off → cal off → pose stand → 等 700ms → dir $j $next")
        flushPendingTrim()
        BleBus.send("pwm off")
        BleBus.send("cal off")
        BleBus.send("pose stand")
        ui.postDelayed({
            dir[j] = next
            BleBus.send("dir $j $next")
            renderAll()
            ui.postDelayed({
                BleBus.send("trimget $j")   // 固件会保持输出脉宽反解新 trim，读回真实值
                dirBusy = false
                renderSlider()
                logSys("CH$j 方向已设为 $next；8 只舵机不该转动。翻转后记得点「保存到板子」。")
            }, 350)
        }, 700)
    }

    private fun wiggle() {
        if (!connected) { toast("先连上 Spider8"); return }
        if (wiggleBusy || dirBusy) { toast("自检正在进行，等它摆完"); return }
        wiggleBusy = true
        val j = cur
        val base = ref[j]
        val hi = base + 25f
        logSys("通电自检：CH$j 在 ${num(base)}° ↔ ${num(hi)}° 之间来回摆 2 次（约 4 秒），请盯住这只舵机。")
        val seq = mutableListOf<Pair<Long, String>>()
        var t = 0L
        repeat(2) {
            seq += t to String.format(Locale.US, "cal %d %.1f", j, hi); t += 900
            seq += t to String.format(Locale.US, "cal %d %.1f", j, base); t += 900
        }
        seq += t to "pose stand"; t += 200
        seq.forEach { (delay, cmd) -> ui.postDelayed({ BleBus.send(cmd) }, delay) }
        ui.postDelayed({
            wiggleBusy = false
            logSys("自检结束，已回站姿。没动 → 查 V+ 5V / 共地 / OE / 线序；动了 → 可以开始拖滑块。")
        }, t)
    }

    private fun zeroAll() {
        flushPendingTrim()
        for (j in 0..7) {
            BleBus.send("trim $j 0.0")
            trim[j] = 0f
        }
        renderAll()
        logSys("8 路 trim 已归零，dir 原样保留（还没 save）。方向不会被翻回去，舵机不该有动作。")
    }

    private fun clearAllWithConfirm() {
        flushPendingTrim()
        logSys("全清：先回站姿，等姿态收敛后再清 trim/dir。")
        BleBus.send("pwm off")
        BleBus.send("cal off")
        BleBus.send("pose stand")
        ui.postDelayed({
            for (j in 0..7) {
                BleBus.send("trim $j 0.0")
                BleBus.send("dir $j 1")
                trim[j] = 0f
                dir[j] = 1
            }
            renderAll()
            logSys("8 路 trim/dir 已恢复默认（还没 save）。")
            ui.postDelayed({ readAll() }, 500)
        }, 700)
    }

    /* ---------------- BLE 回调 ---------------- */

    override fun onLog(msg: String) {
        runOnUiThread { appendLog(msg) }
    }

    override fun onStateChanged(connected: Boolean) {
        this.connected = connected
        updateConnUi(connected)
        if (connected) {
            logSys("已连接，正在读回板子里的 8 路 trim/dir…")
            readAll()
        }
    }

    override fun onStatus(text: String) {
        runOnUiThread {
            appendLog(text.trimEnd())
            parseReply(text)
            if (text.contains("标定值已写入 NVS")) toast("✓ 标定值已写入 NVS，断电不丢")
        }
    }

    private fun parseReply(text: String) {
        var changed = false
        RE_GET.findAll(text).forEach { m ->
            val j = m.groupValues[1].toInt()
            trim[j] = m.groupValues[2].toFloatOrNull()?.coerceIn(-TRIM_LIMIT, TRIM_LIMIT) ?: trim[j]
            dir[j] = m.groupValues[3].toIntOrNull() ?: dir[j]
            changed = true
        }
        RE_TRIM.findAll(text).forEach { m ->
            val j = m.groupValues[1].toInt()
            trim[j] = m.groupValues[2].toFloatOrNull()?.coerceIn(-TRIM_LIMIT, TRIM_LIMIT) ?: trim[j]
            changed = true
        }
        RE_DIR.findAll(text).forEach { m ->
            val j = m.groupValues[1].toInt()
            dir[j] = m.groupValues[2].toIntOrNull() ?: dir[j]
            changed = true
        }
        if (changed) renderAll()
    }

    private fun updateConnUi(c: Boolean) {
        binding.tvCalState.text = if (c) "● 已连接" else "● 未连接"
        binding.tvCalState.setTextColor(
            ContextCompat.getColor(this, if (c) R.color.sp8_accent else R.color.sp8_danger)
        )
        binding.tvCalState.background = ContextCompat.getDrawable(
            this, if (c) R.drawable.bg_badge_on else R.drawable.bg_badge_off
        )
        val gated = listOf<View>(
            binding.chan0, binding.chan1, binding.chan2, binding.chan3,
            binding.chan4, binding.chan5, binding.chan6, binding.chan7,
            binding.seekTrim, binding.btnM1, binding.btnM05, binding.btnZero,
            binding.btnP05, binding.btnP1, binding.btnDir, binding.btnReadAll,
            binding.btnSave, binding.btnStand, binding.btnWiggle, binding.btnPwmOff,
            binding.btnZeroAll, binding.btnClearAll, binding.btnCStatus, binding.btnCPca,
            binding.btnCPwmOff, binding.btnCCalOff, binding.btnCalSend, binding.etCalCmd
        )
        gated.forEach { it.isEnabled = c }
    }

    private fun appendLog(line: String) {
        val curText = binding.tvCalLog.text.toString()
        val next = if (curText.length > 12000) curText.takeLast(8000) + "\n" + line
                   else if (curText.isEmpty()) line else "$curText\n$line"
        binding.tvCalLog.text = next
        binding.scrollCalLog.post { binding.scrollCalLog.fullScroll(View.FOCUS_DOWN) }
    }

    private fun logSys(s: String) = appendLog("[系统] $s")
    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
}
