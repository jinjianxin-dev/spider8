package com.spider8.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Spider8 蓝牙遥控客户端（全局唯一实例，见 [BleBus]）。
 *
 * 板子固件（ble_ctl.c）作为 BLE 外设，设备名 "Spider8"，一个自定义服务两个特征值：
 *   - CMD（写）：手机写 ASCII 命令，板子通过 cli_exec() 执行；
 *   - STS（读 / notify）：命令结果推回手机（严格单包，长输出会被截断）。
 *
 * 和上一版相比的三处关键改动：
 *  ① 多监听器：遥控页与标定页可以同时观察同一个连接，切换 Activity 不必断开重连。
 *  ② 写命令串行排队：安卓 GATT 同时只允许一个未完成的写操作，以前拖滑块 / 连发
 *     8 条 trimget 会静默丢命令；现在等 onCharacteristicWrite 再发下一条，并带超时兜底。
 *  ③ 断连后无限退避重连（用户主动点「断开」除外），长时间遥控偶发 status=8 能自愈。
 */
class BleConnection(private val context: Context) {

    interface Listener {
        /** 普通日志（已切主线程） */
        fun onLog(msg: String)

        /** 连接可用性变化 */
        fun onStateChanged(connected: Boolean)

        /** 收到板子推回的状态文本 */
        fun onStatus(text: String)
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<Listener>()
    private val history = ArrayList<String>()

    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter

    @Volatile private var bluetoothGatt: BluetoothGatt? = null
    @Volatile private var cmdCharacteristic: BluetoothGattCharacteristic? = null
    @Volatile private var stsCharacteristic: BluetoothGattCharacteristic? = null

    @Volatile private var connecting = false
    @Volatile private var ready = false

    /** 用户主动点「断开」时为 true，此时不做自动重连 */
    @Volatile private var userDisconnect = false

    /** 连续自动重连次数，决定退避时长；连上后清零 */
    private var autoRetry = 0

    /** requestMtu 与 discoverServices 必须串行 */
    @Volatile private var discoverStarted = false

    private var scanTimeout: Runnable? = null

    /* ---------------- 写命令串行队列 ---------------- */
    private val pending = ArrayDeque<String>()
    private var writeBusy = false
    private var writeSeq = 0L
    /** 超时兜底已放行的那次写，回调迟到了要丢弃，否则会把队列二次放行 */
    private var missedCallbacks = 0

    companion object {
        private const val TAG = "BleConnection"
        const val DEVICE_NAME = "Spider8"

        // 与固件 ble_ctl.c 的 SPIDER8_*_UUID 严格一致（NimBLE 反序字节 -> 标准 UUID 串）
        val SVC_UUID: UUID = UUID.fromString("4b9131c3-c9c5-cc8f-9e45-b51f01c2af4f")
        val CMD_UUID: UUID = UUID.fromString("4b9131c3-c9c5-cc8f-9e45-b51f02c2af4f")
        val STS_UUID: UUID = UUID.fromString("4b9131c3-c9c5-cc8f-9e45-b51f03c2af4f")
        private val CCC_DESCRIPTOR_UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    /* ---------------- 监听器分发（统一主线程） ---------------- */
    private fun dispatch(block: (Listener) -> Unit) {
        val run = {
            listeners.forEach {
                try { block(it) } catch (e: Exception) { Log.w(TAG, "listener 异常: $e") }
            }
        }
        if (Looper.myLooper() == Looper.getMainLooper()) run() else mainHandler.post(run)
    }

    private fun log(msg: String) {
        history.add(msg)
        while (history.size > 200) history.removeAt(0)
        dispatch { it.onLog(msg) }
    }

    fun addListener(l: Listener) {
        if (listeners.contains(l)) return
        listeners.add(l)
        // 只把历史补发给「新注册」的这个页面；用 dispatch 会连旧页面一起重放，
        // 切页面时旧页面的终端就会出现一整份重复日志。
        val snapshot = history.toList()
        val state = ready
        val replay = Runnable {
            snapshot.forEach { line ->
                try { l.onLog(line) } catch (e: Exception) { Log.w(TAG, "replay 异常: $e") }
            }
            try { l.onStateChanged(state) } catch (e: Exception) { Log.w(TAG, "replay 异常: $e") }
        }
        if (Looper.myLooper() == Looper.getMainLooper()) replay.run()
        else mainHandler.post(replay)
    }

    fun removeListener(l: Listener) {
        listeners.remove(l)
    }

    fun isBluetoothAvailable(): Boolean = bluetoothAdapter != null
    fun isEnabled(): Boolean = bluetoothAdapter?.isEnabled == true
    fun isReady(): Boolean = ready

    /** 开始扫描并自动连接名为 Spider8 的设备 */
    @SuppressLint("MissingPermission")
    fun start() {
        if (connecting || ready) return
        userDisconnect = false
        val scanner: BluetoothLeScanner = bluetoothAdapter?.bluetoothLeScanner ?: run {
            log("蓝牙不可用，请确认已开启")
            return
        }
        connecting = true
        log("正在扫描 Spider8 …")
        val filter = ScanFilter.Builder().setDeviceName(DEVICE_NAME).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            scanner.startScan(listOf(filter), settings, scanCallback)
        } catch (e: Exception) {
            connecting = false
            log("扫描启动失败：${e.message}")
            return
        }
        scanTimeout?.let { mainHandler.removeCallbacks(it) }
        scanTimeout = Runnable {
            if (connecting) {
                log("扫描超时：没找到 Spider8，检查板子是否通电、蓝牙是否开启")
                try { scanner.stopScan(scanCallback) } catch (_: Exception) {}
                connecting = false
                scheduleReconnect()
            }
        }
        mainHandler.postDelayed(scanTimeout!!, 15000)
    }

    /** 主动断开（不触发自动重连） */
    fun stop() {
        userDisconnect = true
        autoRetry = 0
        disconnect()
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            val device: BluetoothDevice = result?.device ?: return
            if (device.name == DEVICE_NAME) {
                scanTimeout?.let { mainHandler.removeCallbacks(it) }
                try { bluetoothAdapter?.bluetoothLeScanner?.stopScan(this) } catch (_: Exception) {}
                log("发现 $DEVICE_NAME，连接中…")
                connect(device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            connecting = false
            log("扫描失败 code=$errorCode（请确认已授予定位/扫描权限）")
            scheduleReconnect()
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        bluetoothGatt = device.connectGatt(context, false, gattCallback)
    }

    /** 把安卓 GATT 断连状态码翻译成人话，方便排错 */
    private fun gattStatusName(status: Int): String = when (status) {
        0    -> "正常完成"
        8    -> "连接超时(8)：射频干扰 / 距离过远 / 手机蓝牙栈忙"
        19   -> "对端主动断开(19)"
        22   -> "本端主动断开(22)"
        133  -> "GATT 通用错误(133)：安卓蓝牙栈内部异常"
        else -> "错误码 $status"
    }

    /** 非主动断开时退避重连：1.5s → 3s → 5s → 8s（上限），连上清零 */
    private fun scheduleReconnect() {
        if (userDisconnect) return
        autoRetry++
        val delay = when {
            autoRetry <= 1 -> 1500L
            autoRetry == 2 -> 3000L
            autoRetry == 3 -> 5000L
            else -> 8000L
        }
        mainHandler.postDelayed({
            if (userDisconnect || connecting || ready || bluetoothGatt != null) return@postDelayed
            log("连接中断，自动重连（第 $autoRetry 次）…")
            start()
        }, delay)
    }

    @SuppressLint("MissingPermission")
    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                autoRetry = 0
                discoverStarted = false
                log("已连接，协商 MTU …")
                // 不要在已连接状态调 requestConnectionPriority / refresh()：
                // 与固件同时发起参数更新或重置 GATT 客户端，都会导致几秒后 status=8。
                try {
                    gatt.requestMtu(256)
                } catch (e: Exception) {
                    Log.w(TAG, "requestMtu 不被支持: $e")
                    if (!discoverStarted) {
                        discoverStarted = true
                        gatt.discoverServices()
                    }
                }
                // 个别机型不回调 onMtuChanged，3 秒后兜底发现服务
                mainHandler.postDelayed({
                    if (!discoverStarted && bluetoothGatt === gatt) {
                        log("MTU 回调超时，直接发现服务")
                        discoverStarted = true
                        gatt.discoverServices()
                    }
                }, 3000)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                connecting = false
                ready = false
                bluetoothGatt = null
                cmdCharacteristic = null
                stsCharacteristic = null
                pending.clear()
                writeBusy = false
                missedCallbacks = 0
                writeSeq++
                try { gatt.close() } catch (e: Exception) { Log.w(TAG, "gatt.close 异常: $e") }
                dispatch { it.onStateChanged(false) }
                log("已断开 status=$status（${gattStatusName(status)}）")
                scheduleReconnect()
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                log("MTU 协商成功：$mtu 字节（固件按实际 MTU 单包回话）")
            } else {
                log("MTU 协商失败 status=$status，仍按默认 23 字节运行")
            }
            if (!discoverStarted) {
                discoverStarted = true
                gatt.discoverServices()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                log("服务发现失败 status=$status，重试连接…")
                gatt.disconnect()
                return
            }
            val svc = gatt.getService(SVC_UUID)
            if (svc == null) {
                log("未找到 Spider8 自定义服务（若刚重刷过固件，请在系统蓝牙里忽略/忘记 Spider8 再连）")
                gatt.disconnect()
                return
            }
            cmdCharacteristic = svc.getCharacteristic(CMD_UUID)
            stsCharacteristic = svc.getCharacteristic(STS_UUID)
            if (cmdCharacteristic == null || stsCharacteristic == null) {
                log("CMD/STS 特征值缺失")
                gatt.disconnect()
                return
            }
            gatt.setCharacteristicNotification(stsCharacteristic, true)
            val ccc = stsCharacteristic!!.getDescriptor(CCC_DESCRIPTOR_UUID)
            if (ccc != null) {
                writeCcc(gatt, ccc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                log("警告：无 CCCD 描述符，可能无法收状态")
            }
            connecting = false
            ready = true
            dispatch { it.onStateChanged(true) }
            log("就绪：可发送指令（与电脑控制台命令一致）")
            // GATT 回调在 Binder 线程，写队列只能由主线程操作
            mainHandler.post { pumpWrite() }   // 连接前缓存的命令在这里补发
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            mainHandler.post {
                if (missedCallbacks > 0) { missedCallbacks--; return@post }
                if (!writeBusy) return@post
                writeBusy = false
                writeSeq++
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    log("发送失败 status=$status（${gattStatusName(status)}）")
                }
                pumpWrite()
            }
        }

        // Android 13+ 走这个三参重载
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid != STS_UUID) return
            val text = String(value, Charsets.UTF_8)
            dispatch { it.onStatus(text) }
        }

        // Android 12 及更早走这个两参重载
        @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return
            if (characteristic.uuid != STS_UUID) return
            val value = characteristic.value ?: return
            val text = String(value, Charsets.UTF_8)
            dispatch { it.onStatus(text) }
        }
    }

    @SuppressLint("MissingPermission")
    private fun writeCcc(
        gatt: BluetoothGatt,
        ccc: BluetoothGattDescriptor,
        value: ByteArray
    ) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(ccc, value)
        } else {
            @Suppress("DEPRECATION")
            ccc.value = value
            @Suppress("DEPRECATION")
            gatt.writeDescriptor(ccc)
        }
    }

    /** 发送一条命令（不带换行；板子按整条写事件执行）。可从任意线程调用。 */
    fun sendCommand(cmd: String) {
        val c = cmd.trim()
        if (c.isEmpty()) return
        mainHandler.post {
            if (pending.size > 40) pending.removeFirst()
            pending.addLast(c)
            pumpWrite()
        }
    }

    /** 必须在主线程调用 */
    @SuppressLint("MissingPermission")
    private fun pumpWrite() {
        if (writeBusy) return
        val gatt = bluetoothGatt
        val char = cmdCharacteristic
        if (gatt == null || char == null) return   // 命令留在队列里，连上再补发
        val cmd = pending.removeFirstOrNull() ?: return
        writeBusy = true
        val seq = ++writeSeq
        log("→ $cmd")
        val data = cmd.toByteArray(Charsets.UTF_8)
        val ok = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeCharacteristic(
                    char, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ) == BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                char.value = data
                @Suppress("DEPRECATION")
                gatt.writeCharacteristic(char)
            }
        } catch (e: Exception) {
            Log.w(TAG, "writeCharacteristic 异常: $e")
            false
        }
        if (!ok) {
            writeBusy = false
            writeSeq++
            pending.addFirst(cmd)
            mainHandler.postDelayed({ pumpWrite() }, 300)
            return
        }
        // 极少数机型不回调 onCharacteristicWrite，超时后放行下一条，避免队列永久卡死。
        // 记一笔「欠回调」，防止这条迟到的回调把刚发的新写又放行一次。
        mainHandler.postDelayed({
            if (writeBusy && writeSeq == seq) {
                writeBusy = false
                writeSeq++
                missedCallbacks++
                pumpWrite()
            }
        }, 8000)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        scanTimeout?.let { mainHandler.removeCallbacks(it) }
        try { bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback) } catch (_: Exception) {}
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
        cmdCharacteristic = null
        stsCharacteristic = null
        pending.clear()
        writeBusy = false
        missedCallbacks = 0
        writeSeq++
        connecting = false
        ready = false
        dispatch { it.onStateChanged(false) }
    }
}
