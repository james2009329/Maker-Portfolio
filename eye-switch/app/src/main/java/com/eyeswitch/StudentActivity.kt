package com.eyeswitch

import android.Manifest
import android.animation.ObjectAnimator
import android.animation.ValueAnimator
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Bundle
import android.os.SystemClock
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import kotlin.math.max

/**
 * Student mode, landscape. Two big targets sit at the ends of the screen; the student
 * looks at one and holds the look, a ring fills, and whatever the teacher chose happens.
 *
 * The same screen runs calibration (MODE_CALIBRATE): it asks the student to look at the
 * middle, then the left target, then the right one, and records where their eyes point
 * for each. With the camera at one end of the phone those three are never symmetrical,
 * which is why calibrating per student matters.
 */
class StudentActivity : AppCompatActivity() {

    private lateinit var settings: Settings
    private lateinit var detector: GestureDetector
    private lateinit var runner: ActionRunner
    private lateinit var tracker: FaceTracker
    private var calibration: Calibration? = null

    private lateinit var root: View
    private lateinit var leftTarget: TargetView
    private lateinit var rightTarget: TargetView
    private lateinit var blinkTarget: TargetView
    private lateinit var centreDot: View
    private lateinit var instruction: TextView
    private lateinit var faceDot: View
    private lateinit var faceText: TextView
    private lateinit var counterText: TextView
    private lateinit var overlay: View

    private val calibrating get() = intent.getStringExtra(EXTRA_MODE) == MODE_CALIBRATE
    private var finished = false

    // Session tallies for the summary.
    private val sessionStart = SystemClock.elapsedRealtime()
    private val counts = mutableMapOf<Gesture, Int>()
    private val total get() = counts.values.sum()

    private var lastHadFace: Boolean? = null

    private val requestCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        if (ok) tracker.start() else showMessage(R.string.error_permission)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_student)
        UiKit.immersive(this)

        settings = Settings(this)
        detector = GestureDetector(settings)
        calibration = settings.calibration

        root = findViewById(R.id.root)
        leftTarget = findViewById(R.id.leftTarget)
        rightTarget = findViewById(R.id.rightTarget)
        blinkTarget = findViewById(R.id.blinkTarget)
        centreDot = findViewById(R.id.centreDot)
        instruction = findViewById(R.id.instructionText)
        faceDot = findViewById(R.id.faceDot)
        faceText = findViewById(R.id.faceText)
        counterText = findViewById(R.id.counterText)
        overlay = findViewById(R.id.overlay)
        runner = ActionRunner(this, findViewById(R.id.lightsView))

        setUpTargets()

        findViewById<MaterialButton>(R.id.doneButton).setOnClickListener { leave() }
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = leave()
        })

        tracker = FaceTracker(this, null, { settings.useBackCamera }, ::onFrame, ::showMessage)

        if (calibrating) startCalibration()

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            tracker.start()
        } else {
            requestCamera.launch(Manifest.permission.CAMERA)
        }
    }

    private fun setUpTargets() {
        val style = settings.targetStyle
        leftTarget.apply { gesture = Gesture.LOOK_LEFT; this.style = style; pointsLeft = true }
        rightTarget.apply { gesture = Gesture.LOOK_RIGHT; this.style = style; pointsLeft = false }
        blinkTarget.apply { gesture = Gesture.BLINK; this.style = style }

        if (calibrating) {
            counterText.visibility = View.GONE
            blinkTarget.visibility = View.GONE
            return
        }
        leftTarget.active = settings.isEnabled(Gesture.LOOK_LEFT)
        rightTarget.active = settings.isEnabled(Gesture.LOOK_RIGHT)
        blinkTarget.visibility = if (settings.isEnabled(Gesture.BLINK)) View.VISIBLE else View.GONE
    }

    override fun onResume() {
        super.onResume()
        UiKit.immersive(this)
        EyeSwitchApp.of(this).usb.connect()
    }

    override fun onPause() {
        super.onPause()
        runner.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        runner.release()
    }

    // ---------------------------------------------------------------- frames

    private fun onFrame(frame: FaceFrame) {
        if (finished) return
        val now = frame.timeMs
        val scores = frame.scores
        showFace(scores != null)

        if (calibrating) {
            val h = scores?.let { Blendshapes.horizontal(it) }
            onCalibrationSample(h, now)
            return
        }

        if (scores == null) {
            detector.onNoFace()
            setRings(null, 0f)
            return
        }

        val signals = Blendshapes.toSignals(scores, settings, calibration)
        val fired = detector.onFace(signals, now)
        setRings(detector.current, detector.holdProgress)

        if (fired != null) {
            runner.perform(fired, settings, root)
            targetFor(fired).celebrate()
            counts[fired] = (counts[fired] ?: 0) + 1
            counterText.text = total.toString()
            pop(counterText)
        }
    }

    private fun setRings(current: Gesture?, progress: Float) {
        leftTarget.setProgress(if (current == Gesture.LOOK_LEFT) progress else 0f)
        rightTarget.setProgress(if (current == Gesture.LOOK_RIGHT) progress else 0f)
        blinkTarget.setProgress(if (current == Gesture.BLINK) progress else 0f)
    }

    private fun targetFor(g: Gesture) = when (g) {
        Gesture.LOOK_LEFT -> leftTarget
        Gesture.LOOK_RIGHT -> rightTarget
        Gesture.BLINK -> blinkTarget
    }

    private fun showFace(hasFace: Boolean) {
        if (lastHadFace == hasFace) return
        lastHadFace = hasFace
        faceDot.backgroundTintList = ColorStateList.valueOf(if (hasFace) Palette.OK else Palette.MUTED)
        faceText.setText(if (hasFace) R.string.student_watching else R.string.student_looking)
    }

    private fun pop(v: View) {
        v.animate().scaleX(1.15f).scaleY(1.15f).setDuration(140).withEndAction {
            v.animate().scaleX(1f).scaleY(1f).setDuration(220).start()
        }.start()
    }

    // ---------------------------------------------------------------- calibration

    private enum class Step { CENTRE, LEFT, RIGHT }

    private var step = Step.CENTRE
    private var stepStart = 0L
    private val samples = mutableMapOf<Step, MutableList<Float>>()
    private var pulse: ObjectAnimator? = null

    private fun startCalibration() {
        samples.clear()
        setStep(Step.CENTRE)
    }

    private fun setStep(s: Step) {
        step = s
        stepStart = SystemClock.uptimeMillis()
        samples[s] = mutableListOf()
        instruction.visibility = View.VISIBLE
        instruction.setText(
            when (s) {
                Step.CENTRE -> R.string.cal_centre
                Step.LEFT -> R.string.cal_left
                Step.RIGHT -> R.string.cal_right
            }
        )
        centreDot.visibility = if (s == Step.CENTRE) View.VISIBLE else View.GONE
        leftTarget.active = s == Step.LEFT
        rightTarget.active = s == Step.RIGHT
        leftTarget.setProgress(0f)
        rightTarget.setProgress(0f)

        pulse?.cancel()
        if (s == Step.CENTRE) {
            pulse = ObjectAnimator.ofFloat(centreDot, View.ALPHA, 1f, 0.45f).apply {
                duration = 900
                repeatMode = ValueAnimator.REVERSE
                repeatCount = ValueAnimator.INFINITE
                start()
            }
        }
    }

    /** Called every frame while calibrating, with the sideways eye reading or null. */
    private fun onCalibrationSample(h: Float?, now: Long) {
        val elapsed = now - stepStart
        val settle = if (step == Step.CENTRE) SETTLE_FIRST_MS else SETTLE_MS
        val list = samples[step] ?: return

        if (elapsed > settle && h != null) list += h

        // Fill the active target's ring as the samples come in.
        val fraction = (list.size / NEEDED_SAMPLES.toFloat()).coerceIn(0f, 1f)
        when (step) {
            Step.LEFT -> leftTarget.setProgress(fraction)
            Step.RIGHT -> rightTarget.setProgress(fraction)
            Step.CENTRE -> Unit
        }

        val enough = list.size >= NEEDED_SAMPLES && elapsed > settle + SAMPLE_MS
        if (enough) {
            when (step) {
                Step.CENTRE -> setStep(Step.LEFT)
                Step.LEFT -> { leftTarget.celebrate(); setStep(Step.RIGHT) }
                Step.RIGHT -> { rightTarget.celebrate(); finishCalibration() }
            }
        } else if (elapsed > STEP_TIMEOUT_MS) {
            calibrationFailed()
        }
    }

    private fun finishCalibration() {
        val c = Calibration(
            median(samples[Step.CENTRE]!!),
            median(samples[Step.LEFT]!!),
            median(samples[Step.RIGHT]!!)
        )
        if (!c.isValid) {
            calibrationFailed()
            return
        }
        settings.saveCalibration(c)
        finished = true
        pulse?.cancel()
        instruction.visibility = View.GONE
        centreDot.visibility = View.GONE
        showOverlay(
            title = getString(R.string.cal_done_title),
            big = "",
            bigLabel = "",
            body = getString(R.string.cal_done_body),
            primary = getString(R.string.done) to { finish() },
            secondary = getString(R.string.cal_again) to { restartCalibration() }
        )
    }

    private fun calibrationFailed() {
        finished = true
        pulse?.cancel()
        showOverlay(
            title = getString(R.string.cal_failed_title),
            big = "",
            bigLabel = "",
            body = getString(R.string.cal_failed_body),
            primary = getString(R.string.cal_again) to { restartCalibration() },
            secondary = getString(R.string.cancel) to { finish() }
        )
    }

    private fun restartCalibration() {
        overlay.visibility = View.GONE
        finished = false
        startCalibration()
    }

    private fun median(list: List<Float>): Float {
        val s = list.sorted()
        return if (s.isEmpty()) 0f else s[s.size / 2]
    }

    // ---------------------------------------------------------------- leaving

    private fun leave() {
        if (calibrating || finished) {
            finish()
            return
        }
        finished = true
        runner.stop()
        settings.lastSessionCount = total
        settings.lastSessionAtMs = System.currentTimeMillis()

        val minutes = max(1L, (SystemClock.elapsedRealtime() - sessionStart) / 60_000L).toInt()
        showOverlay(
            title = getString(R.string.summary_title, ProfileStore(this).currentName),
            big = total.toString(),
            bigLabel = resources.getQuantityString(R.plurals.activations_word, total),
            body = resources.getQuantityString(R.plurals.minutes, minutes, minutes),
            primary = getString(R.string.summary_home) to { finish() },
            secondary = getString(R.string.summary_keep_going) to {
                overlay.visibility = View.GONE
                finished = false
            }
        )
        fillStats()
    }

    private fun fillStats() {
        val row = findViewById<LinearLayout>(R.id.statRow)
        row.removeAllViews()
        for (g in Gesture.values()) {
            if (!settings.isEnabled(g)) continue
            val chip = LayoutInflater.from(this).inflate(R.layout.item_stat, row, false)
            chip.findViewById<View>(R.id.statDot).backgroundTintList = ColorStateList.valueOf(Palette.of(g))
            chip.findViewById<TextView>(R.id.statValue).text = (counts[g] ?: 0).toString()
            chip.findViewById<TextView>(R.id.statLabel).text = UiKit.gestureName(this, g)
            row.addView(chip)
        }
    }

    private fun showOverlay(
        title: String,
        big: String,
        bigLabel: String,
        body: String,
        primary: Pair<String, () -> Unit>,
        secondary: Pair<String, () -> Unit>
    ) {
        findViewById<TextView>(R.id.overlayTitle).text = title
        findViewById<TextView>(R.id.overlayBig).apply { text = big; visibility = if (big.isEmpty()) View.GONE else View.VISIBLE }
        findViewById<TextView>(R.id.overlayBigLabel).text = bigLabel
        findViewById<TextView>(R.id.overlayBody).text = body
        findViewById<LinearLayout>(R.id.statRow).removeAllViews()
        findViewById<MaterialButton>(R.id.overlayPrimary).apply {
            text = primary.first
            setOnClickListener { primary.second() }
        }
        findViewById<MaterialButton>(R.id.overlaySecondary).apply {
            text = secondary.first
            setOnClickListener { secondary.second() }
        }
        overlay.alpha = 0f
        overlay.visibility = View.VISIBLE
        overlay.animate().alpha(1f).setDuration(300).start()
    }

    private fun showMessage(res: Int) {
        instruction.visibility = View.VISIBLE
        instruction.setText(res)
    }

    companion object {
        const val EXTRA_MODE = "mode"
        const val MODE_CALIBRATE = "calibrate"

        private const val SETTLE_FIRST_MS = 2000L
        private const val SETTLE_MS = 1200L
        private const val SAMPLE_MS = 1500L
        private const val NEEDED_SAMPLES = 15
        private const val STEP_TIMEOUT_MS = 9000L

        fun calibrate(context: Context) = Intent(context, StudentActivity::class.java)
            .putExtra(EXTRA_MODE, MODE_CALIBRATE)
    }
}
