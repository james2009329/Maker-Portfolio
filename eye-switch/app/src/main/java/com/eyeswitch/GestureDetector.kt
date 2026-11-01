package com.eyeswitch

import kotlin.math.abs
import kotlin.math.sign

/**
 * One camera frame's worth of readings.
 *
 * blink      - how shut both eyes are, 0-1 (the less-shut eye, so both must close)
 * lookLeft   - progress towards the left target, 0-1
 * lookRight  - progress towards the right target, 0-1
 * horizontal - the raw sideways eye turn, kept so calibration can sample it
 */
data class FaceSignals(
    val blink: Float,
    val lookLeft: Float,
    val lookRight: Float,
    val horizontal: Float
)

/**
 * Where the eyes point while the student looks at the middle, the left target and the
 * right target, all on the same raw scale. Mapping live readings between these three
 * points means "1.0" always means "looking at that target", for every student and for
 * whichever end of the phone the camera is on.
 */
data class Calibration(val centre: Float, val left: Float, val right: Float) {

    /** Left and right must sit on opposite sides of the middle, a clear distance apart. */
    val isValid: Boolean
        get() {
            val dl = left - centre
            val dr = right - centre
            return abs(dl) >= MIN_SPREAD && abs(dr) >= MIN_SPREAD && sign(dl) != sign(dr)
        }

    fun leftProgress(h: Float) = ((h - centre) / (left - centre)).coerceIn(0f, 1f)
    fun rightProgress(h: Float) = ((h - centre) / (right - centre)).coerceIn(0f, 1f)

    companion object {
        const val MIN_SPREAD = 0.08f
    }
}

/**
 * Turns MediaPipe's face "blendshape" scores into FaceSignals.
 *
 * MediaPipe reports, for each eye, how far it is turned inward (towards the nose) and
 * outward (towards the ear). Looking to your own right turns your right eye outward and
 * your left eye inward, so those are averaged into one sideways number: positive for
 * right, negative for left. Using both eyes makes it steadier than either alone.
 */
object Blendshapes {

    fun horizontal(scores: Map<String, Float>): Float {
        fun s(name: String) = scores[name] ?: 0f
        val right = (s("eyeLookOutRight") + s("eyeLookInLeft")) / 2f
        val left = (s("eyeLookOutLeft") + s("eyeLookInRight")) / 2f
        return right - left
    }

    fun toSignals(scores: Map<String, Float>, settings: Settings, calibration: Calibration?): FaceSignals {
        fun s(name: String) = scores[name] ?: 0f
        val blink = minOf(s("eyeBlinkLeft"), s("eyeBlinkRight"))
        val h = horizontal(scores)

        if (calibration != null) {
            return FaceSignals(blink, calibration.leftProgress(h), calibration.rightProgress(h), h)
        }

        // Not calibrated yet: use the raw scores, with a manual swap for cameras that
        // report the other way round.
        var right = (s("eyeLookOutRight") + s("eyeLookInLeft")) / 2f
        var left = (s("eyeLookOutLeft") + s("eyeLookInRight")) / 2f
        if (settings.swapLeftRight) {
            val t = left
            left = right
            right = t
        }
        return FaceSignals(blink, left, right, h)
    }
}

/**
 * Decides when a held gesture counts as a deliberate signal.
 *
 * The rule is the same for every gesture: it must be held continuously for
 * holdDurationMs, then it fires ONCE. It cannot fire again until the student lets go
 * and refractoryMs has passed. A gesture whose action is "Nothing" is ignored entirely.
 */
class GestureDetector(private val settings: Settings) {

    enum class State { NO_FACE, READY, HOLDING, FIRED }

    @Volatile var state: State = State.NO_FACE
        private set

    /** The gesture currently being held, or null. */
    @Volatile var current: Gesture? = null
        private set

    /** 0.0 to 1.0 - how far through the hold the student is. Drives the fill rings. */
    @Volatile var holdProgress: Float = 0f
        private set

    private var heldSinceMs = 0L
    private var firedForThisHold = false
    private var lastFiredAtMs = 0L

    /** Call when a frame has no face. Resets everything so progress cannot be banked. */
    fun onNoFace() {
        current = null
        firedForThisHold = false
        holdProgress = 0f
        state = State.NO_FACE
    }

    /** Call once per frame with a face. Returns the gesture to act on, or null. */
    fun onFace(signals: FaceSignals, nowMs: Long): Gesture? {
        val candidate = pickGesture(signals)

        // Switching gesture - or letting go - restarts the clock.
        if (candidate != current) {
            current = candidate
            heldSinceMs = nowMs
            firedForThisHold = false
        }

        if (candidate == null) {
            holdProgress = 0f
            state = State.READY
            return null
        }

        val heldFor = nowMs - heldSinceMs
        val hold = settings.holdDurationMs
        holdProgress = (heldFor.toFloat() / hold.toFloat()).coerceIn(0f, 1f)

        val longEnough = heldFor >= hold
        val outOfRefractory = (nowMs - lastFiredAtMs) >= settings.refractoryMs

        if (longEnough && outOfRefractory && !firedForThisHold) {
            firedForThisHold = true
            lastFiredAtMs = nowMs
            state = State.FIRED
            return candidate
        }

        state = if (firedForThisHold) State.FIRED else State.HOLDING
        return null
    }

    private fun pickGesture(s: FaceSignals): Gesture? {
        // With the eyes shut there is no iris to read, so any gaze reading is noise.
        if (s.blink >= settings.blinkThreshold) {
            return if (settings.isEnabled(Gesture.BLINK)) Gesture.BLINK else null
        }

        val threshold = settings.gazeThreshold
        val left = s.lookLeft >= threshold && settings.isEnabled(Gesture.LOOK_LEFT)
        val right = s.lookRight >= threshold && settings.isEnabled(Gesture.LOOK_RIGHT)

        return when {
            left && right -> if (s.lookLeft >= s.lookRight) Gesture.LOOK_LEFT else Gesture.LOOK_RIGHT
            left -> Gesture.LOOK_LEFT
            right -> Gesture.LOOK_RIGHT
            else -> null
        }
    }

    /** Clears everything, e.g. after the teacher changes a setting. */
    fun reset() {
        onNoFace()
        lastFiredAtMs = 0L
    }
}
