package com.eyeswitch

import android.Manifest
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Bundle
import android.view.View
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator

/**
 * Set up, for staff. Shows the camera with an oval to fit the student's face into, warns
 * if the room is too dark, runs calibration, shows a live left-right reading, and has
 * one-tap buttons to try each look's action without the student.
 */
class SetupActivity : AppCompatActivity() {

    private lateinit var settings: Settings
    private lateinit var tracker: FaceTracker
    private lateinit var runner: ActionRunner

    private lateinit var faceGuide: FaceGuideView
    private lateinit var guideText: TextView
    private lateinit var gazeMeter: GazeMeterView
    private lateinit var blinkMeter: LinearProgressIndicator
    private lateinit var calibrationTag: TextView
    private lateinit var statusPill: View

    private var lastGuide = -1

    private val usbListener: (UsbSerialManager.Status) -> Unit = { UiKit.renderStatus(statusPill, it) }

    private val requestCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        if (ok) tracker.start() else guideText.setText(R.string.error_permission)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_setup)

        settings = Settings(this)
        faceGuide = findViewById(R.id.faceGuide)
        guideText = findViewById(R.id.guideText)
        gazeMeter = findViewById(R.id.gazeMeter)
        blinkMeter = findViewById(R.id.blinkMeter)
        calibrationTag = findViewById(R.id.calibrationTag)
        statusPill = findViewById(R.id.statusPill)
        runner = ActionRunner(this, findViewById(R.id.lightsView))

        findViewById<TextView>(R.id.profileText).text = ProfileStore(this).currentName
        findViewById<View>(R.id.backButton).setOnClickListener { finish() }

        findViewById<MaterialButton>(R.id.calibrateButton).setOnClickListener {
            UiKit.tick(it)
            startActivity(StudentActivity.calibrate(this))
        }

        bindTest(R.id.testLeft, Gesture.LOOK_LEFT)
        bindTest(R.id.testRight, Gesture.LOOK_RIGHT)
        bindTest(R.id.testBlink, Gesture.BLINK)

        findViewById<MaterialSwitch>(R.id.rearCameraSwitch).apply {
            isChecked = settings.useBackCamera
            setOnCheckedChangeListener { v, checked ->
                UiKit.tick(v)
                settings.useBackCamera = checked
                tracker.rebindIfCameraChanged()
            }
        }

        tracker = FaceTracker(this, findViewById<PreviewView>(R.id.previewView),
            { settings.useBackCamera }, ::onFrame) { guideText.setText(it) }

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            tracker.start()
        } else {
            requestCamera.launch(Manifest.permission.CAMERA)
        }
    }

    private fun bindTest(id: Int, g: Gesture) {
        findViewById<MaterialButton>(id).setOnClickListener {
            if (!settings.isEnabled(g)) return@setOnClickListener
            UiKit.tick(it)
            runner.perform(g, settings, it)
        }
    }

    override fun onResume() {
        super.onResume()
        // Settings or calibration may have changed on another screen.
        UiKit.renderCalibration(calibrationTag, settings)
        refreshTestButtons()
        findViewById<View>(R.id.blinkRow).visibility =
            if (settings.isEnabled(Gesture.BLINK)) View.VISIBLE else View.GONE
        val app = EyeSwitchApp.of(this)
        app.addUsbListener(usbListener)
        app.usb.connect()
    }

    override fun onPause() {
        super.onPause()
        EyeSwitchApp.of(this).removeUsbListener(usbListener)
        runner.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        runner.release()
    }

    /** A look set to "Nothing" has nothing to test, so its button sits dim. */
    private fun refreshTestButtons() {
        for ((id, g) in listOf(R.id.testLeft to Gesture.LOOK_LEFT, R.id.testRight to Gesture.LOOK_RIGHT, R.id.testBlink to Gesture.BLINK)) {
            findViewById<View>(id).apply {
                alpha = if (settings.isEnabled(g)) 1f else 0.35f
                visibility = if (g == Gesture.BLINK && !settings.isEnabled(g)) View.GONE else View.VISIBLE
            }
        }
    }

    private fun onFrame(frame: FaceFrame) {
        val scores = frame.scores
        val placed = FaceGuideView.isWellPlaced(frame.faceBox)
        faceGuide.good = placed

        val guide = when {
            frame.brightness < FaceTracker.DARK_THRESHOLD -> R.string.guide_dark
            scores == null -> R.string.guide_no_face
            !placed -> R.string.guide_adjust
            else -> R.string.guide_good
        }
        if (guide != lastGuide) {
            lastGuide = guide
            guideText.setText(guide)
            val colour = when (guide) {
                R.string.guide_good -> Palette.OK
                R.string.guide_dark -> Palette.WARN
                else -> Palette.TEXT
            }
            guideText.setTextColor(colour)
            guideText.backgroundTintList = ColorStateList.valueOf(Palette.BACKGROUND)
        }

        if (scores == null) {
            gazeMeter.update(0f, 0f, settings.gazeThreshold, false)
            blinkMeter.setProgressCompat(0, true)
            return
        }
        val s = Blendshapes.toSignals(scores, settings, settings.calibration)
        gazeMeter.update(s.lookLeft, s.lookRight, settings.gazeThreshold, true)
        blinkMeter.setProgressCompat((s.blink * 100).toInt(), true)
    }
}
