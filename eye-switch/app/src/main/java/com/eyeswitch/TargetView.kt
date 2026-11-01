package com.eyeswitch

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.SystemClock
import android.util.AttributeSet
import android.view.View
import kotlin.math.PI
import kotlin.math.min
import kotlin.math.sin

/**
 * One big round target in student mode. The student looks at it, and a ring fills around
 * it while they hold the look. When it fires, the target gives a slow bloom of its own
 * colour.
 *
 * Everything is flat colour and every movement is slow - a gentle breathing pulse, an
 * easing ring and a ripple lasting well over a second - so nothing flashes.
 */
class TargetView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    var gesture: Gesture = Gesture.LOOK_LEFT
        set(v) { field = v; invalidate() }

    var style: TargetStyle = TargetStyle.COLOURS
        set(v) { field = v; invalidate() }

    /** Arrows point outwards, towards the side of the screen the target sits on. */
    var pointsLeft: Boolean = true
        set(v) { field = v; invalidate() }

    /** When false the target sits dim, so the student is not drawn to it. */
    var active: Boolean = true
        set(v) {
            field = v
            animate().alpha(if (v) 1f else DIM_ALPHA).setDuration(400).start()
        }

    private var targetProgress = 0f
    private var shownProgress = 0f
    private val ripples = ArrayList<Long>()
    private var bounceStart = 0L

    private val dp = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }
    private val emoji = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val arc = RectF()
    private val arrow = Path()

    fun setProgress(p: Float) {
        targetProgress = p.coerceIn(0f, 1f)
        invalidate()
    }

    /** The reward bloom. Two soft rings spread out and fade over about 1.4 seconds. */
    fun celebrate() {
        val now = SystemClock.uptimeMillis()
        ripples += now
        ripples += now + RIPPLE_GAP_MS
        bounceStart = now
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val now = SystemClock.uptimeMillis()
        val cx = width / 2f
        val cy = height / 2f
        val base = min(width, height) / 2f * 0.60f
        val colour = Palette.of(gesture)

        // Ease the ring towards its target so it glides rather than jumps.
        shownProgress += (targetProgress - shownProgress) * 0.22f
        if (kotlin.math.abs(targetProgress - shownProgress) < 0.002f) shownProgress = targetProgress

        // Ripples, drawn first so they sit behind the disc.
        val iter = ripples.iterator()
        while (iter.hasNext()) {
            val start = iter.next()
            if (now < start) continue
            val t = (now - start) / RIPPLE_MS.toFloat()
            if (t >= 1f) { iter.remove(); continue }
            fill.color = colour
            fill.alpha = ((1f - t) * 110).toInt()
            canvas.drawCircle(cx, cy, base * (1f + 0.62f * easeOut(t)), fill)
        }

        // Breathing pulse while idle, a short bounce when it fires.
        var scale = 1f
        if (shownProgress < 0.02f && active) {
            scale += 0.03f * sin(2 * PI * (now % PULSE_MS) / PULSE_MS).toFloat()
        }
        val bt = (now - bounceStart) / BOUNCE_MS.toFloat()
        if (bounceStart > 0 && bt in 0f..1f) scale += 0.10f * sin(PI * bt).toFloat()
        val r = base * scale

        // Track and progress ring.
        val ringR = base + 16 * dp
        ring.strokeWidth = 9 * dp
        ring.color = Palette.SURFACE_RAISED
        canvas.drawCircle(cx, cy, ringR, ring)
        if (shownProgress > 0.005f) {
            ring.color = colour
            arc.set(cx - ringR, cy - ringR, cx + ringR, cy + ringR)
            canvas.drawArc(arc, -90f, 360f * shownProgress, false, ring)
        }

        // The disc itself.
        fill.color = colour
        fill.alpha = 255
        canvas.drawCircle(cx, cy, r, fill)

        when (style) {
            TargetStyle.COLOURS -> {
                fill.color = Palette.partnerOf(gesture)
                canvas.drawCircle(cx, cy, r * 0.42f, fill)
            }
            TargetStyle.ANIMALS -> {
                emoji.textSize = r * 1.05f
                val y = cy - (emoji.descent() + emoji.ascent()) / 2f
                canvas.drawText(animalFor(gesture), cx, y, emoji)
            }
            TargetStyle.ARROWS -> drawArrow(canvas, cx, cy, r)
        }

        // Keep animating while anything is moving.
        if (active || ripples.isNotEmpty() || shownProgress != targetProgress) postInvalidateOnAnimation()
    }

    private fun drawArrow(canvas: Canvas, cx: Float, cy: Float, r: Float) {
        val s = r * 0.5f
        val dir = if (gesture == Gesture.BLINK) 0f else if (pointsLeft) -1f else 1f
        fill.color = Palette.BACKGROUND
        fill.alpha = 230
        if (dir == 0f) {
            canvas.drawCircle(cx, cy, s * 0.45f, fill)
            return
        }
        arrow.reset()
        // A chunky arrow: shaft plus head, pointing in dir.
        val tip = cx + dir * s
        val back = cx - dir * s
        val neck = cx + dir * s * 0.05f
        val shaft = s * 0.26f
        arrow.moveTo(tip, cy)
        arrow.lineTo(neck, cy - s * 0.75f)
        arrow.lineTo(neck, cy - shaft)
        arrow.lineTo(back, cy - shaft)
        arrow.lineTo(back, cy + shaft)
        arrow.lineTo(neck, cy + shaft)
        arrow.lineTo(neck, cy + s * 0.75f)
        arrow.close()
        canvas.drawPath(arrow, fill)
    }

    private fun easeOut(t: Float) = 1f - (1f - t) * (1f - t)

    companion object {
        private const val PULSE_MS = 2400L
        private const val RIPPLE_MS = 1400L
        private const val RIPPLE_GAP_MS = 320L
        private const val BOUNCE_MS = 600L
        const val DIM_ALPHA = 0.28f

        fun animalFor(g: Gesture) = when (g) {
            Gesture.LOOK_LEFT -> "🐳"   // whale
            Gesture.LOOK_RIGHT -> "🦁"  // lion
            Gesture.BLINK -> "🦉"       // owl
        }
    }
}
