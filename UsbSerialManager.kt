package com.eyeswitch

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber

/**
 * Talks to the ESP32 over the phone's USB-C port in host (OTG) mode, through the board's
 * own USB-to-serial chip (CP2102 or CH340), so nothing else sits between phone and board.
 *
 * Protocol: one line per press, "A<pulseMs>\n" or "B<pulseMs>\n", e.g. "A250\n". The
 * pulse length travels with each press, so it can change without reflashing the sketch.
 */
class UsbSerialManager(
    private val context: Context,
    private val onStatusChanged: (Status) -> Unit
) {

    enum class Status { DISCONNECTED, NO_DEVICE, PERMISSION_DENIED, CONNECTED }

    private val usbManager: UsbManager =
        context.getSystemService(Context.USB_SERVICE) as UsbManager

    private var port: UsbSerialPort? = null

    /** Ask once per plug-in, so cancelling the prompt does not bring it straight back. */
    private var permissionAsked = false

    var status: Status = Status.DISCONNECTED
        private set(value) {
            field = value
            onStatusChanged(value)
        }

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context?, intent: Intent?) {
            if (intent?.action != ACTION_USB_PERMISSION) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (granted) {
                openPort()
            } else {
                Log.w(TAG, "User denied USB permission")
                status = Status.PERMISSION_DENIED
            }
        }
    }

    fun register() {
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            context.registerReceiver(permissionReceiver, filter)
        }
    }

    fun unregister() {
        try {
            context.unregisterReceiver(permissionReceiver)
        } catch (e: IllegalArgumentException) {
            // Receiver was not registered. Harmless.
        }
        close()
    }

    /**
     * Finds the ESP32 and connects. If Android has not yet been granted permission for
     * this device it shows the system prompt first and connects from the receiver above.
     */
    fun connect() {
        // Already open - nothing to do. A dead port is cleared on the next failed write.
        if (port != null) {
            status = Status.CONNECTED
            return
        }
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        if (drivers.isEmpty()) {
            permissionAsked = false
            status = Status.NO_DEVICE
            return
        }

        val device = drivers[0].device
        if (usbManager.hasPermission(device)) {
            openPort()
        } else {
            if (permissionAsked) return
            permissionAsked = true
            // Must be mutable so Android can add whether permission was granted.
            val flags =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE
                else 0
            val pi = PendingIntent.getBroadcast(
                context, 0, Intent(ACTION_USB_PERMISSION).setPackage(context.packageName), flags
            )
            usbManager.requestPermission(device, pi)
        }
    }

    private fun openPort() {
        try {
            val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
            if (drivers.isEmpty()) {
                status = Status.NO_DEVICE
                return
            }
            val driver = drivers[0]
            val connection = usbManager.openDevice(driver.device)
            if (connection == null) {
                status = Status.PERMISSION_DENIED
                return
            }
            val p = driver.ports[0]
            p.open(connection)
            p.setParameters(BAUD_RATE, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            // DTR high keeps many boards from sitting in reset.
            try {
                p.dtr = true
                p.rts = true
            } catch (e: UnsupportedOperationException) {
                // Some drivers do not expose these. Not fatal.
            }
            port = p
            status = Status.CONNECTED
            Log.i(TAG, "Connected to ${driver.device.deviceName}")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to open serial port", e)
            port = null
            status = Status.DISCONNECTED
        }
    }

    /**
     * Presses one of the two switch outputs. Safe to call from any thread.
     * If the cable was pulled out this quietly marks itself disconnected rather than
     * crashing, and the next connect() call will pick the board up again.
     *
     * @param channel 'A' or 'B' - which relay, and so which 3.5 mm jack
     */
    fun sendPulse(channel: Char, pulseMs: Int) {
        val p = port
        if (p == null) {
            status = Status.DISCONNECTED
            return
        }
        try {
            p.write("$channel$pulseMs\n".toByteArray(Charsets.US_ASCII), WRITE_TIMEOUT_MS)
        } catch (e: Exception) {
            Log.e(TAG, "Write failed, dropping connection", e)
            close()
            status = Status.DISCONNECTED
        }
    }

    fun close() {
        try {
            port?.close()
        } catch (e: Exception) {
            // Already gone.
        }
        port = null
    }

    companion object {
        private const val TAG = "UsbSerialManager"
        private const val ACTION_USB_PERMISSION = "com.eyeswitch.USB_PERMISSION"
        private const val BAUD_RATE = 115200
        private const val WRITE_TIMEOUT_MS = 200
    }
}
