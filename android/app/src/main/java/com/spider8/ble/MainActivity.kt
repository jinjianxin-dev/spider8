package com.spider8.ble

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.spider8.ble.databinding.ActivityMainBinding
import java.util.Locale

/**
 * 遥控主页 —— 按钮集合与 firmware/main/web/console.html 完全一致：
 *   急停 / 语音动作开关 / 动作(6) / 行走转向(7) / 速度·转向 / 高级参数(4 滑块) / 命令终端。
 * 每条按钮发出的都是同一张固件命令表里的字符串，不做任何二次翻译。
 *
 * 语音开关：固件 I2 起上电默认 exec=1（喊一声就会走），所以主页必须能
 * 在「放地上 / 搬动 / 收纳」前一键 `voice exec off`。状态用 `voice` 的
 * cli_out 短回话读回（含 exec=0/1），不靠本地猜测。
 */
class MainActivity : AppCompatActivity(), BleConnection.Listener {

    private lateinit var binding: ActivityMainBinding
    private lateinit var ble: BleConnection
    private var connected = false
    private var lastState: Boolean? = null
    private var voiceExecOn: Boolean? = null      /* null=还没读回过固件状态 */

    private val requiredPermissions: Array<String>
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.BLUETOOTH,
                Manifest.permission.BLUETOOTH_ADMIN
            )
        }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) ensureBluetoothOn { ble.start() }
        else toast("缺少蓝牙权限，无法连接")
    }

    private val enableBtLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        if (ble.isEnabled()) ble.start() else toast("请先开启蓝牙")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ble = BleBus.get(this)
        setupUi()
        bindSliders()
        updateConnUi(BleBus.isReady())
        appendLog("Spider8 遥控已就绪 · 命令与电脑控制台一致")
    }

    override fun onStart() {
        super.onStart()
        ble.addListener(this)
        updateConnUi(BleBus.isReady())
    }

    override fun onStop() {
        ble.removeListener(this)
        super.onStop()
    }

    private fun setupUi() {
        binding.btnConnect.setOnClickListener {
            if (connected) ble.stop() else checkPermissionsThenConnect()
        }
        val openCalib = View.OnClickListener {
            startActivity(Intent(this, CalibActivity::class.java))
        }
        binding.btnCalib.setOnClickListener(openCalib)
        binding.btnCalibOpen.setOnClickListener(openCalib)

        // 按钮文字与 console.html 的 data-cmd 一一对应
        val buttons: Map<View, String> = mapOf(
            binding.btnEstop       to "stop",
            binding.btnPoseStand   to "pose stand",
            binding.btnPoseCrouch  to "pose crouch",
            binding.btnPoseLie     to "pose lie",
            binding.btnPoseShake   to "pose shake",
            binding.btnPoseWave    to "pose wave",
            binding.btnDemo        to "demo",
            binding.btnWalk        to "walk 100",
            binding.btnTrot        to "trot 100",
            binding.btnLeft        to "left",
            binding.btnRight       to "right",
            binding.btnStraight    to "straight",
            binding.btnStopWalk    to "stop",
            binding.btnQStatus     to "status",
            binding.btnQPca        to "pcadiag",
            binding.btnQPwmOff     to "pwm off",
            binding.btnQCalOff     to "cal off"
        )
        buttons.forEach { (v, cmd) -> v.setOnClickListener { BleBus.send(cmd) } }

        /* 后退绝对不能发 walk -50：motion.c 的 execute_pending() 里
         * `if (s_req_speed >= 0)` 会跳过负速度，motion_set_speed() 又把它夹成 0，
         * 结果是用上一次的速度继续【前进】。改成点了只解释、不发命令。 */
        binding.btnBackward.setOnClickListener {
            toast("后退暂未开放：固件会把负速度当成继续前进，已禁用")
        }

        /* 语音动作开关：null（未知）时只允许往安全的「关」方向走。
         * 发完 exec 命令再发一条 voice 读回，避免本地状态与固件脱节。 */
        binding.btnVoiceExec.setOnClickListener {
            val turnOn = voiceExecOn == false
            BleBus.send(if (turnOn) "voice exec on" else "voice exec off")
            voiceExecOn = turnOn
            updateVoiceUi()
            BleBus.send("voice")
        }
        binding.btnVoiceStatus.setOnClickListener { BleBus.send("voice") }
        updateVoiceUi()

        binding.btnSend.setOnClickListener { sendTyped() }
        binding.etCmd.setOnEditorActionListener { _, _, _ -> sendTyped(); true }
        binding.btnClear.setOnClickListener {
            binding.tvLog.text = ""
            appendLog("终端已清空")
        }
    }

    private fun sendTyped() {
        val cmd = binding.etCmd.text.toString().trim()
        if (cmd.isEmpty()) return
        BleBus.send(cmd)
        binding.etCmd.text?.clear()
    }

    /** 滑块只在松手时发一条命令，与网页 bindSlider 的行为一致 */
    private fun bindSliders() {
        attachSlider(binding.seekSpeed, binding.tvSpeed, 100) { p ->
            "$p%" to "speed $p"
        }
        attachSlider(binding.seekTurn, binding.tvTurn, 100) { p ->
            val v = p - 100
            "$v" to "turn $v"
        }
        attachSlider(binding.seekPeriod, binding.tvPeriod, 8) { p ->
            val v = 800 + p * 50
            "$v" to "period $v"
        }
        attachSlider(binding.seekStride, binding.tvStride, 28) { p ->
            val v = 8f + p * 0.5f
            String.format(Locale.US, "%.1f", v) to String.format(Locale.US, "stride %.1f", v)
        }
        attachSlider(binding.seekLift, binding.tvLift, 16) { p ->
            val v = 6f + p * 0.5f
            String.format(Locale.US, "%.1f", v) to String.format(Locale.US, "lift %.1f", v)
        }
        attachSlider(binding.seekDuty, binding.tvDuty, 15) { p ->
            val v = 10 + p
            "$v%" to "duty $v"
        }
    }

    private fun attachSlider(
        seek: SeekBar,
        label: TextView,
        defProgress: Int,
        map: (Int) -> Pair<String, String>
    ) {
        label.text = map(defProgress).first
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                label.text = map(p).first
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {
                BleBus.send(map(sb?.progress ?: defProgress).second)
            }
        })
    }

    private fun checkPermissionsThenConnect() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) ensureBluetoothOn { ble.start() }
        else permLauncher.launch(missing.toTypedArray())
    }

    private fun ensureBluetoothOn(action: () -> Unit) {
        if (!ble.isBluetoothAvailable()) {
            toast("此设备不支持蓝牙")
            return
        }
        if (ble.isEnabled()) action()
        else enableBtLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
    }

    /* ---- BleConnection.Listener ---- */

    override fun onLog(msg: String) {
        runOnUiThread { appendLog(msg) }
    }

    override fun onStateChanged(connected: Boolean) {
        this.connected = connected
        if (lastState != connected) {
            lastState = connected
            if (connected) {
                appendLog("● 已连接 Spider8")
                /* 稍等 GATT 服务发现完成，再读回 exec/mute 同步开关文案 */
                binding.root.postDelayed({ if (this.connected) BleBus.send("voice") }, 800)
            } else {
                voiceExecOn = null
                updateVoiceUi()
            }
        }
        updateConnUi(connected)
    }

    override fun onStatus(text: String) {
        val body = text.trimEnd()
        /* `voice` 的 cli_out 短回话含 "exec=0/1"，用它校准本地开关，
         * 避免手机显示的状态与固件不一致。 */
        val hit = Regex("exec=([01])").find(body)
        runOnUiThread {
            if (hit != null) {
                voiceExecOn = hit.groupValues[1] == "1"
                updateVoiceUi()
            }
            appendLog(body)
        }
    }

    /** 只改文案；enabled 统一由 updateConnUi 按连接状态管 */
    private fun updateVoiceUi() {
        binding.btnVoiceExec.text = if (voiceExecOn == false) "开启语音动作" else "关闭语音动作"
        binding.tvVoiceState.text = when (voiceExecOn) {
            true  -> "语音动作：已放行（喊「前进」会走；放地上前请关闭）"
            false -> "语音动作：已关闭（只识别、不动腿）"
            else  -> "语音动作：未知（连上后自动读取，或点「查询状态」）"
        }
    }

    private fun updateConnUi(c: Boolean) {
        binding.btnConnect.text = if (c) "断开" else "连接 Spider8"
        binding.tvState.text = if (c) "● 已连接" else "● 未连接"
        binding.tvState.setTextColor(
            ContextCompat.getColor(this, if (c) R.color.sp8_accent else R.color.sp8_danger)
        )
        binding.tvState.background = ContextCompat.getDrawable(
            this, if (c) R.drawable.bg_badge_on else R.drawable.bg_badge_off
        )

        val gated = listOf<View>(
            binding.btnEstop, binding.btnPoseStand, binding.btnPoseCrouch,
            binding.btnPoseLie, binding.btnPoseShake, binding.btnPoseWave, binding.btnDemo,
            binding.btnWalk, binding.btnTrot, binding.btnBackward, binding.btnLeft,
            binding.btnRight, binding.btnStraight, binding.btnStopWalk,
            binding.btnVoiceExec, binding.btnVoiceStatus,
            binding.seekSpeed, binding.seekTurn, binding.seekPeriod, binding.seekStride,
            binding.seekLift, binding.seekDuty, binding.btnQStatus, binding.btnQPca,
            binding.btnQPwmOff, binding.btnQCalOff, binding.btnSend, binding.etCmd,
            binding.btnCalibOpen
        )
        gated.forEach { it.isEnabled = c }
    }

    private fun appendLog(line: String) {
        val cur = binding.tvLog.text.toString()
        val next = if (cur.length > 12000) cur.takeLast(8000) + "\n" + line
                   else if (cur.isEmpty()) line else "$cur\n$line"
        binding.tvLog.text = next
        binding.scrollLog.post { binding.scrollLog.fullScroll(View.FOCUS_DOWN) }
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
}
