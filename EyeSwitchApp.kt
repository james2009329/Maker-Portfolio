package com.eyeswitch

import android.app.Application
import android.content.Context
import android.os.Handler
import android.os.Looper

/**
 * Holds the one USB connection to the switch box for the whole app, so moving between
 * the home, set-up and student screens never drops the connection. Screens subscribe to
 * hear when the box is plugged in or pulled out.
 */
class EyeSwitchApp : Application() {

    lateinit var usb: UsbSerialManager
        private set

    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableSetOf<(UsbSerialManager.Status) -> Unit>()

    override fun onCreate() {
        super.onCreate()
        usb = UsbSerialManager(this) { status ->
            main.post { listeners.toList().forEach { it(status) } }
        }
        usb.register()
    }

    fun addUsbListener(l: (UsbSerialManager.Status) -> Unit) {
        listeners += l
        l(usb.status)
    }

    fun removeUsbListener(l: (UsbSerialManager.Status) -> Unit) {
        listeners -= l
    }

    companion object {
        fun of(context: Context) = context.applicationContext as EyeSwitchApp
    }
}
