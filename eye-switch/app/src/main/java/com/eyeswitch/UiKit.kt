package com.eyeswitch

import android.app.Activity
import android.content.res.ColorStateList
import android.text.format.DateUtils
import android.view.HapticFeedbackConstants
import android.view.View
import android.widget.ImageView
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.widget.TextViewCompat

/** Small shared pieces of UI, so every screen shows things the same way. */
object UiKit {

    /** The "Toy connected" / "Phone only" pill. */
    fun renderStatus(pill: View, status: UsbSerialManager.Status) {
        val icon = pill.findViewById<ImageView>(R.id.statusIcon)
        val text = pill.findViewById<TextView>(R.id.statusText)
        val ctx = pill.context
        val (iconRes, colour, label) = when (status) {
            UsbSerialManager.Status.CONNECTED ->
                Triple(R.drawable.ic_plug, Palette.OK, R.string.status_connected)
            UsbSerialManager.Status.PERMISSION_DENIED ->
                Triple(R.drawable.ic_plug, Palette.WARN, R.string.status_permission)
            else ->
                Triple(R.drawable.ic_phone, Palette.MUTED, R.string.status_phone_only)
        }
        icon.setImageResource(iconRes)
        icon.imageTintList = ColorStateList.valueOf(colour)
        text.text = ctx.getString(label)
    }

    /** The "Calibrated" / "Not calibrated" tag. */
    fun renderCalibration(tag: TextView, settings: Settings) {
        val ctx = tag.context
        val done = settings.calibration != null
        val colour = if (done) Palette.OK else Palette.WARN
        val icon = ContextCompat.getDrawable(ctx, if (done) R.drawable.ic_check else R.drawable.ic_target)!!.mutate()
        val size = (16 * ctx.resources.displayMetrics.density).toInt()
        icon.setBounds(0, 0, size, size)
        tag.setCompoundDrawablesRelative(icon, null, null, null)
        TextViewCompat.setCompoundDrawableTintList(tag, ColorStateList.valueOf(colour))
        tag.setTextColor(colour)
        tag.text = if (done) {
            ctx.getString(
                R.string.calibrated_when,
                DateUtils.getRelativeTimeSpanString(
                    settings.calibratedAtMs, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS
                )
            )
        } else ctx.getString(R.string.not_calibrated)
    }

    /** A light tick under the finger, so changes feel like they registered. */
    fun tick(v: View) {
        v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    /** Hides the status and navigation bars, for student mode. A swipe brings them back. */
    fun immersive(activity: Activity) {
        WindowCompat.setDecorFitsSystemWindows(activity.window, false)
        WindowInsetsControllerCompat(activity.window, activity.window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    fun gestureName(activity: Activity, g: Gesture): String = activity.getString(
        when (g) {
            Gesture.LOOK_LEFT -> R.string.gesture_left
            Gesture.LOOK_RIGHT -> R.string.gesture_right
            Gesture.BLINK -> R.string.gesture_blink
        }
    )
}
