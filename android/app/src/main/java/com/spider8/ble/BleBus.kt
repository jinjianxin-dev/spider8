package com.spider8.ble

import android.content.Context

/**
 * 全局唯一的 BLE 连接。
 *
 * 遥控页和 trim 标定页是两个 Activity；如果各自 new 一个 BleConnection，
 * 切页面就会把 GATT 断开重连（用户看到「又断了一次」）。这里用 Application
 * Context 持有实例，Activity 只在 onStart/onStop 里注册/注销监听器，
 * ACL 链路在页面切换期间保持不变。
 */
object BleBus {

    @Volatile
    private var instance: BleConnection? = null

    fun get(context: Context): BleConnection =
        instance ?: synchronized(this) {
            instance ?: BleConnection(context.applicationContext).also { instance = it }
        }

    fun isReady(): Boolean = instance?.isReady() == true

    fun send(cmd: String) {
        instance?.sendCommand(cmd)
    }
}
