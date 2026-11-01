package com.eyeswitch

import android.content.Intent
import android.content.res.ColorStateList
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import android.transition.AutoTransition
import android.transition.TransitionManager
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.chip.ChipGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.slider.Slider
import kotlin.math.roundToInt

/**
 * Settings for the current student. The everyday choices come first - what each look
 * does, how sensitive it is, what the targets look like - and the fine-tuning sliders
 * sit under Advanced. Every change saves straight away.
 */
class SettingsActivity : AppCompatActivity() {

    private lateinit var settings: Settings
    private lateinit var previewLeft: TargetView
    private lateinit var previewRight: TargetView

    private var pickingFor: Gesture? = null
    private val songLabels = mutableMapOf<Gesture, TextView>()

    private val pickSong = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val g = pickingFor ?: return@registerForActivityResult
        pickingFor = null
        if (uri == null) return@registerForActivityResult
        // Keep access to the file after the phone restarts.
        try {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            // Some file providers do not offer lasting access. It still works this session.
        }
        settings.setSong(g, uri)
        updateSongLabel(g)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        settings = Settings(this)
        findViewById<TextView>(R.id.profileText).text = ProfileStore(this).currentName
        findViewById<View>(R.id.backButton).setOnClickListener { finish() }

        val container = findViewById<LinearLayout>(R.id.gestureContainer)
        for (g in listOf(Gesture.LOOK_LEFT, Gesture.LOOK_RIGHT, Gesture.BLINK)) addGestureRow(container, g)

        bindSensitivity()
        bindStyle()
        bindReward()
        bindAdvanced()
    }

    // ---------------------------------------------------------------- what each look does

    private val options = listOf(
        R.id.optNone to Action.NONE,
        R.id.optSwitch to Action.SWITCH_A,
        R.id.optSong to Action.SONG,
        R.id.optLights to Action.LIGHTS,
        R.id.optBoth to Action.SONG_AND_LIGHTS
    )

    private fun addGestureRow(container: LinearLayout, g: Gesture) {
        val row = LayoutInflater.from(this).inflate(R.layout.item_gesture, container, false)
        container.addView(row)

        row.findViewById<View>(R.id.colorDot).backgroundTintList = ColorStateList.valueOf(Palette.of(g))
        row.findViewById<TextView>(R.id.gestureTitle).text = UiKit.gestureName(this, g)
        val actionName = row.findViewById<TextView>(R.id.actionName)
        val songRow = row.findViewById<View>(R.id.songRow)
        songLabels[g] = row.findViewById(R.id.songLabel)
        updateSongLabel(g)

        val group = row.findViewById<MaterialButtonToggleGroup>(R.id.actionGroup)
        val current = settings.actionFor(g).let { if (it == Action.SWITCH_B) Action.SWITCH_A else it }
        group.check(options.first { it.second == current }.first)
        actionName.text = describe(current)
        songRow.visibility = if (current.playsSong) View.VISIBLE else View.GONE

        group.addOnButtonCheckedListener { _, id, checked ->
            if (!checked) return@addOnButtonCheckedListener
            val action = options.first { it.first == id }.second
            settings.setAction(g, action)
            UiKit.tick(group)
            actionName.text = describe(action)
            TransitionManager.beginDelayedTransition(row as android.view.ViewGroup, AutoTransition().setDuration(180))
            songRow.visibility = if (action.playsSong) View.VISIBLE else View.GONE
            refreshPreview()
        }

        row.findViewById<View>(R.id.chooseSongButton).setOnClickListener {
            pickingFor = g
            pickSong.launch(arrayOf("audio/*"))
        }
        row.findViewById<View>(R.id.builtInButton).setOnClickListener {
            settings.setSong(g, null)
            updateSongLabel(g)
        }
    }

    private fun describe(a: Action) = getString(
        when (a) {
            Action.NONE -> R.string.action_none
            Action.SWITCH_A, Action.SWITCH_B -> R.string.action_switch
            Action.SONG -> R.string.action_song
            Action.LIGHTS -> R.string.action_lights
            Action.SONG_AND_LIGHTS -> R.string.action_both
        }
    )

    private fun updateSongLabel(g: Gesture) {
        val label = songLabels[g] ?: return
        val uri = settings.songFor(g)
        label.text = if (uri == null) getString(R.string.song_built_in) else getString(R.string.song_chosen, displayName(uri))
    }

    private fun displayName(uri: Uri): String {
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { c -> if (c.moveToFirst()) return c.getString(0) }
        } catch (e: Exception) {
            // Fall through to the raw path.
        }
        return uri.lastPathSegment ?: uri.toString()
    }

    // ---------------------------------------------------------------- sensitivity

    private val sensChips = listOf(
        R.id.sensEasy to Sensitivity.EASY,
        R.id.sensMedium to Sensitivity.MEDIUM,
        R.id.sensHard to Sensitivity.HARD
    )

    private fun bindSensitivity() {
        val group = findViewById<ChipGroup>(R.id.sensitivityGroup)
        val help = findViewById<TextView>(R.id.sensitivityHelp)
        fun show() {
            val match = Sensitivity.matching(settings.gazeThreshold)
            group.setOnCheckedStateChangeListener(null)
            if (match == null) group.clearCheck() else group.check(sensChips.first { it.second == match }.first)
            help.setText(
                when (match) {
                    Sensitivity.EASY -> R.string.sens_easy_help
                    Sensitivity.MEDIUM -> R.string.sens_medium_help
                    Sensitivity.HARD -> R.string.sens_hard_help
                    null -> R.string.sens_custom_help
                }
            )
            group.setOnCheckedStateChangeListener { g, ids ->
                val id = ids.firstOrNull() ?: return@setOnCheckedStateChangeListener
                settings.gazeThreshold = sensChips.first { it.first == id }.second.threshold
                UiKit.tick(g)
                show()
                syncGazeSlider()
            }
        }
        show()
        refreshSensitivity = ::show
    }

    private var refreshSensitivity: () -> Unit = {}
    private var syncGazeSlider: () -> Unit = {}

    // ---------------------------------------------------------------- target style

    private val styleChips = listOf(
        R.id.styleColours to TargetStyle.COLOURS,
        R.id.styleAnimals to TargetStyle.ANIMALS,
        R.id.styleArrows to TargetStyle.ARROWS
    )

    private fun bindStyle() {
        previewLeft = findViewById(R.id.previewLeft)
        previewRight = findViewById(R.id.previewRight)
        previewLeft.apply { gesture = Gesture.LOOK_LEFT; pointsLeft = true }
        previewRight.apply { gesture = Gesture.LOOK_RIGHT; pointsLeft = false }

        val group = findViewById<ChipGroup>(R.id.styleGroup)
        group.check(styleChips.first { it.second == settings.targetStyle }.first)
        group.setOnCheckedStateChangeListener { g, ids ->
            val id = ids.firstOrNull() ?: return@setOnCheckedStateChangeListener
            settings.targetStyle = styleChips.first { it.first == id }.second
            UiKit.tick(g)
            refreshPreview()
            previewLeft.celebrate()
            previewRight.celebrate()
        }
        refreshPreview()
    }

    private fun refreshPreview() {
        val style = settings.targetStyle
        previewLeft.style = style
        previewRight.style = style
        previewLeft.active = settings.isEnabled(Gesture.LOOK_LEFT)
        previewRight.active = settings.isEnabled(Gesture.LOOK_RIGHT)
    }

    // ---------------------------------------------------------------- song and lights

    private val rewardChips = listOf(R.id.rewardShort, R.id.rewardMedium, R.id.rewardLong)

    private fun bindReward() {
        val group = findViewById<ChipGroup>(R.id.rewardGroup)
        val idx = Settings.REWARD_CHOICES_MS.indexOf(settings.rewardMs)
        if (idx >= 0) group.check(rewardChips[idx])
        group.setOnCheckedStateChangeListener { g, ids ->
            val id = ids.firstOrNull() ?: return@setOnCheckedStateChangeListener
            settings.rewardMs = Settings.REWARD_CHOICES_MS[rewardChips.indexOf(id)]
            UiKit.tick(g)
        }

        findViewById<MaterialSwitch>(R.id.soundSwitch).apply {
            isChecked = settings.soundEnabled
            setOnCheckedChangeListener { v, checked -> settings.soundEnabled = checked; UiKit.tick(v) }
        }
    }

    // ---------------------------------------------------------------- advanced

    private fun bindAdvanced() {
        val header = findViewById<View>(R.id.advancedHeader)
        val card = findViewById<View>(R.id.advancedCard)
        val chevron = findViewById<View>(R.id.advancedChevron)
        header.setOnClickListener {
            val open = card.visibility != View.VISIBLE
            TransitionManager.beginDelayedTransition(card.parent as android.view.ViewGroup, AutoTransition().setDuration(220))
            card.visibility = if (open) View.VISIBLE else View.GONE
            chevron.animate().rotation(if (open) 180f else 0f).setDuration(220).start()
            UiKit.tick(it)
        }

        // Further right always means "easier for the student to trigger".
        bindSlider("gaze", 5f, 95f, 1f, ((1f - settings.gazeThreshold) * 100f).coerceIn(5f, 95f).roundToInt().toFloat(),
            R.string.label_gaze, R.string.help_gaze) {
            settings.gazeThreshold = 1f - it / 100f
            refreshSensitivity()
        }
        syncGazeSlider = {
            findViewById<Slider>(R.id.gazeSlider).value =
                ((1f - settings.gazeThreshold) * 100f).coerceIn(5f, 95f).roundToInt().toFloat()
        }
        syncGazeSlider()

        bindSlider("blink", 5f, 95f, 1f, ((1f - settings.blinkThreshold) * 100f).coerceIn(5f, 95f).roundToInt().toFloat(),
            R.string.label_blink, R.string.help_blink) { settings.blinkThreshold = 1f - it / 100f }

        bindSlider("hold", Settings.MIN_HOLD_MS.toFloat(), Settings.MAX_HOLD_MS.toFloat(), 50f,
            snap(settings.holdDurationMs.toFloat(), Settings.MIN_HOLD_MS.toFloat(), 50f),
            R.string.label_hold, R.string.help_hold) { settings.holdDurationMs = it.toLong() }

        bindSlider("gap", Settings.MIN_REFRACTORY_MS.toFloat(), Settings.MAX_REFRACTORY_MS.toFloat(), 100f,
            snap(settings.refractoryMs.toFloat(), Settings.MIN_REFRACTORY_MS.toFloat(), 100f),
            R.string.label_refractory, R.string.help_refractory) { settings.refractoryMs = it.toLong() }

        bindSlider("pulse", Settings.MIN_PULSE_MS.toFloat(), Settings.MAX_PULSE_MS.toFloat(), 50f,
            snap(settings.pulseMs.toFloat(), Settings.MIN_PULSE_MS.toFloat(), 50f),
            R.string.label_pulse, R.string.help_pulse) { settings.pulseMs = it.toInt() }

        findViewById<MaterialSwitch>(R.id.swapSwitch).apply {
            isChecked = settings.swapLeftRight
            setOnCheckedChangeListener { v, checked -> settings.swapLeftRight = checked; UiKit.tick(v) }
        }

        findViewById<View>(R.id.clearCalibrationButton).setOnClickListener {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.button_clear_calibration)
                .setMessage(R.string.clear_calibration_body)
                .setPositiveButton(R.string.button_clear_calibration) { _, _ -> settings.clearCalibration() }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
        findViewById<View>(R.id.resetButton).setOnClickListener {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.button_reset)
                .setMessage(R.string.reset_body)
                .setPositiveButton(R.string.button_reset) { _, _ ->
                    settings.resetToDefaults()
                    recreate()
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }

    /** Slider values must land exactly on a step, or the Material slider throws. */
    private fun snap(v: Float, from: Float, step: Float) = from + Math.round((v - from) / step) * step

    private fun bindSlider(
        name: String, from: Float, to: Float, step: Float, initial: Float,
        labelRes: Int, helpRes: Int, onChange: (Float) -> Unit
    ) {
        fun id(suffix: String) = resources.getIdentifier("$name$suffix", "id", packageName)
        val slider = findViewById<Slider>(id("Slider"))
        val label = findViewById<TextView>(id("Label"))
        findViewById<TextView>(id("Help")).setText(helpRes)

        slider.valueFrom = from
        slider.valueTo = to
        slider.stepSize = step
        slider.value = snap(initial.coerceIn(from, to), from, step)
        label.text = getString(labelRes, slider.value.toInt())

        slider.addOnChangeListener { s, value, fromUser ->
            label.text = getString(labelRes, value.toInt())
            if (fromUser) {
                onChange(value)
                s.performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK)
            }
        }
    }
}
