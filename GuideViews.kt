package com.eyeswitch

import android.content.Context
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View

/**
 * Drawn over the camera preview on the set-up screen: an oval to fit the student's face
 * into. Grey and dashed while it is still looking, solid green once the face is centred
 * and a sensible size.
 */
class FaceGuideView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    var good: Boolean = false
        set(v) { if (field != v) { field = v; invalidate() } }

    private val dp = resources.displayMetrics.density
    private val shade = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Palette.BACKGROUND; alpha = 120 }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val oval = RectF()
    private val path = Path()

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val ow = minOf(w * 0.42f, h * 0.62f)
        val oh = ow * 1.3f
        oval.set((w - ow) / 2, (h - oh) / 2, (w + ow) / 2, (h + oh) / 2)

        // Shade everything outside the oval.
        path.reset()
        path.fillType = Path.FillType.EVEN_ODD
        path.addRect(0f, 0f, w, h, Path.Direction.CW)
        path.addOval(oval, Path.Direction.CW)
        canvas.drawPath(path, shade)

        stroke.strokeWidth = 4 * dp
        stroke.color = if (good) Palette.OK else Palette.TEXT
        stroke.pathEffect = if (good) null else DashPathEffect(floatArrayOf(14 * dp, 10 * dp), 0f)
        canvas.drawOval(oval, stroke)
    }

    companion object {
        /** Face centred and filling a sensible part of the frame. Box is 0-1 on each axis. */
        fun isWellPlaced(box: RectF?): Boolean {
            if (box == null) return false
            val cx = box.centerX()
            val cy = box.centerY()
            val size = box.width()
            return cx in 0.3f..0.7f && cy in 0.25f..0.75f && size in 0.18f..0.8f
        }
    }
}

/**
 * A live left-right meter for the set-up screen. The dot moves left or right with the
 * student's eyes; the small ticks show where a look starts to count.
 */
class GazeMeterView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    private var left = 0f
    private var right = 0f
    private var threshold = 0.55f
    private var hasFace = false

    private val dp = resources.displayMetrics.density
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()

    fun update(lookLeft: Float, lookRight: Float, threshold: Float, hasFace: Boolean) {
        left = lookLeft
        right = lookRight
        this.threshold = threshold
        this.hasFace = hasFace
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val cy = height / 2f
        val pad = 10 * dp
        val half = (w - 2 * pad) / 2
        val cx = w / 2
        val trackH = 12 * dp

        paint.color = Palette.SURFACE_RAISED
        rect.set(pad, cy - trackH / 2, w - pad, cy + trackH / 2)
        canvas.drawRoundRect(rect, trackH / 2, trackH / 2, paint)

        // Fill from the centre towards whichever side is stronger.
        val value = if (right >= left) right else -left
        if (hasFace && kotlin.math.abs(value) > 0.01f) {
            paint.color = Palette.of(if (value < 0) Gesture.LOOK_LEFT else Gesture.LOOK_RIGHT)
            val end = cx + value * half
            rect.set(minOf(cx, end), cy - trackH / 2, maxOf(cx, end), cy + trackH / 2)
            canvas.drawRoundRect(rect, trackH / 2, trackH / 2, paint)
        }

        // Threshold ticks and centre mark.
        paint.color = Palette.TEXT
        paint.strokeWidth = 2 * dp
        for (x in floatArrayOf(cx - threshold * half, cx + threshold * half)) {
            canvas.drawLine(x, cy - trackH, x, cy + trackH, paint)
        }
        paint.color = Palette.MUTED
        canvas.drawLine(cx, cy - trackH * 0.7f, cx, cy + trackH * 0.7f, paint)

        if (hasFace) {
            paint.color = Palette.TEXT
            canvas.drawCircle(cx + value * half, cy, 9 * dp, paint)
        }
    }
}
