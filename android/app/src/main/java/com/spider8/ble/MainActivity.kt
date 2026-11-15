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
 *   急停 / 动作(6) / 行走转向(7) / 速度·转向 / 高级参数(4 滑块) / 命令终端。
 * 每条按钮发出的都是同一张固件命令表里的字符串，不做任何二次翻译。
 */
class MainActivity : AppCompatActivity(), BleConnection.Listener {

    private lateinit var binding: ActivityMainBinding
    private lateinit var ble: BleConnection
    private var connected = false
    private var lastState: Boolean? = null

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
            binding.btnBackward    to "walk -50",
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
            if (connected) appendLog("● 已连接 Spider8")
        }
        updateConnUi(connected)
    }

    override fun onStatus(text: String) {
        runOnUiThread { appendLog(text.trimEnd()) }
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
