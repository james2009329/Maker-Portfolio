package com.eyeswitch

import android.graphics.Color

/**
 * The app's colours, for code that draws by hand. Flat colours only - no gradients.
 * The same values are in res/values/colors.xml for layouts.
 */
object Palette {
    val BACKGROUND = Color.parseColor("#0F1B2D")
    val SURFACE = Color.parseColor("#18283F")
    val SURFACE_RAISED = Color.parseColor("#22344F")
    val TEXT = Color.parseColor("#F3EFE6")
    val MUTED = Color.parseColor("#93A1B5")
    val OK = Color.parseColor("#6CC48A")
    val WARN = Color.parseColor("#E8A66D")

    /** Each gesture keeps one colour everywhere, so a student can learn which is which. */
    fun of(g: Gesture): Int = when (g) {
        Gesture.LOOK_LEFT -> Color.parseColor("#3FB8AF")   // teal
        Gesture.LOOK_RIGHT -> Color.parseColor("#F2B84B")  // amber
        Gesture.BLINK -> Color.parseColor("#A99BDB")       // lavender
    }

    /** A lighter partner colour the lights drift towards. */
    fun partnerOf(g: Gesture): Int = when (g) {
        Gesture.LOOK_LEFT -> Color.parseColor("#8AD9D2")
        Gesture.LOOK_RIGHT -> Color.parseColor("#F7D58E")
        Gesture.BLINK -> Color.parseColor("#CFC5EE")
    }
}
