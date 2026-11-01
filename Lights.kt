package com.eyeswitch

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.animation.ArgbEvaluator
import android.animation.ValueAnimator
import android.view.View
import android.view.animation.LinearInterpolator

/**
 * Fills the screen with colour as a reward.
 *
 * SAFETY - photosensitive epilepsy. Some students with multiple disabilities have
 * seizure disorders that flashing light can trigger. The accessibility guideline
 * (WCAG 2.3.1) forbids more than three flashes in any one second. This class never
 * flashes at all: the screen fades in over FADE_IN_MS, drifts slowly between two
 * related colours taking DRIFT_MS each way, and fades out over FADE_OUT_MS.
 *
 * These are constants, not settings, on purpose - nobody should be able to speed them
 * up by accident. The colours are soft rather than saturated, and there is no red.
 */
class Lights(private val view: View) {

    private var animator: ValueAnimator? = null
    private val evaluator = ArgbEvaluator()

    fun show(gesture: Gesture, lengthMs: Long) {
        cancel()
        val (from, to) = colours(gesture)
        val total = lengthMs.toFloat()

        view.setBackgroundColor(from)
        view.alpha = 0f
        view.visibility = View.VISIBLE

        val anim = ValueAnimator.ofFloat(0f, total)
        anim.duration = lengthMs
        anim.interpolator = LinearInterpolator()
        anim.addUpdateListener { va ->
            val t = va.animatedValue as Float
            val alpha = when {
                t < FADE_IN_MS -> t / FADE_IN_MS
                t > total - FADE_OUT_MS -> ((total - t) / FADE_OUT_MS).coerceIn(0f, 1f)
                else -> 1f
            }
            // One slow sweep from one colour to the other and back every 2 x DRIFT_MS.
            val phase = (t % (2 * DRIFT_MS)) / DRIFT_MS
            val mix = if (phase <= 1f) phase else 2f - phase
            view.setBackgroundColor(evaluator.evaluate(smooth(mix), from, to) as Int)
            view.alpha = alpha * MAX_ALPHA
        }
        anim.addListener(object : AnimatorListenerAdapter() {
            override fun onAnimationEnd(animation: Animator) {
                view.visibility = View.GONE
            }
        })
        anim.start()
        animator = anim
    }

    fun cancel() {
        animator?.cancel()
        animator = null
        view.visibility = View.GONE
    }

    /** Each gesture gets its own colours, so the student can learn which is which. */
    private fun colours(g: Gesture): Pair<Int, Int> = Palette.of(g) to Palette.partnerOf(g)

    private fun smooth(x: Float) = x * x * (3f - 2f * x)

    companion object {
        private const val FADE_IN_MS = 600f
        private const val FADE_OUT_MS = 900f
        private const val DRIFT_MS = 2500f
        /** Slightly see-through so the teacher can still check the camera view. */
        private const val MAX_ALPHA = 0.85f
    }
}
