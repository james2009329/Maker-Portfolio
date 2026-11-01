package com.eyeswitch

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri

/** The three things a student can do on purpose to make something happen. */
enum class Gesture(val key: String) {
    LOOK_LEFT("left"),
    LOOK_RIGHT("right"),
    BLINK("blink")
}

/**
 * What a gesture does when it fires. The teacher picks one per gesture.
 * SWITCH_B is kept so a second relay can be added later, but the settings screen
 * only offers one switch since the current box has one relay.
 */
enum class Action {
    NONE,
    SWITCH_A,
    SWITCH_B,
    SONG,
    LIGHTS,
    SONG_AND_LIGHTS;

    val playsSong get() = this == SONG || this == SONG_AND_LIGHTS
    val showsLights get() = this == LIGHTS || this == SONG_AND_LIGHTS
    val pressesSwitch get() = this == SWITCH_A || this == SWITCH_B
}

/** Plain choice for how small a glance counts. Each maps to one gaze threshold. */
enum class Sensitivity(val threshold: Float) {
    EASY(0.40f),
    MEDIUM(0.55f),
    HARD(0.70f);

    companion object {
        fun matching(threshold: Float): Sensitivity? =
            values().firstOrNull { kotlin.math.abs(it.threshold - threshold) < 0.005f }
    }
}

/** What the two on-screen targets look like in student mode. */
enum class TargetStyle { COLOURS, ANIMALS, ARROWS }

/**
 * Every tunable value for ONE student lives here, so the screens and the detector read
 * from exactly one place. Each student profile gets its own file. Everything is saved the
 * moment it changes.
 */
class Settings(context: Context, val profileId: String = ProfileStore(context).currentId) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences(prefsName(profileId), Context.MODE_PRIVATE)

    // ---- What each gesture does ----

    fun actionFor(g: Gesture): Action {
        val stored = prefs.getString("action_${g.key}", null) ?: return defaultAction(g)
        return runCatching { Action.valueOf(stored) }.getOrDefault(defaultAction(g))
    }

    fun setAction(g: Gesture, action: Action) {
        prefs.edit().putString("action_${g.key}", action.name).apply()
    }

    fun isEnabled(g: Gesture) = actionFor(g) != Action.NONE

    /** A song the teacher picked from the phone, or null for the built-in tune. */
    fun songFor(g: Gesture): Uri? = prefs.getString("song_${g.key}", null)?.let(Uri::parse)

    fun setSong(g: Gesture, uri: Uri?) {
        val editor = prefs.edit()
        if (uri == null) editor.remove("song_${g.key}") else editor.putString("song_${g.key}", uri.toString())
        editor.apply()
    }

    // ---- Detection ----

    /** Blink score (0-1) the eyes must reach to count as shut. Lower = more sensitive. */
    var blinkThreshold: Float
        get() = prefs.getFloat("blink_threshold", DEFAULT_BLINK_THRESHOLD)
        set(v) = prefs.edit().putFloat("blink_threshold", v).apply()

    /**
     * How far to the side the eyes must look, 0-1. After calibration, 1.0 means "looking
     * right at the target", so 0.55 means a little over halfway there.
     */
    var gazeThreshold: Float
        get() = prefs.getFloat("gaze_threshold", Sensitivity.MEDIUM.threshold)
        set(v) = prefs.edit().putFloat("gaze_threshold", v).apply()

    /** How long a gesture must be held before it fires. Filters out ordinary blinks and glances. */
    var holdDurationMs: Long
        get() = prefs.getLong("hold_duration_ms", DEFAULT_HOLD_MS)
        set(v) = prefs.edit().putLong("hold_duration_ms", v).apply()

    /** Minimum gap between two activations, so one long hold cannot fire repeatedly. */
    var refractoryMs: Long
        get() = prefs.getLong("refractory_ms", DEFAULT_REFRACTORY_MS)
        set(v) = prefs.edit().putLong("refractory_ms", v).apply()

    /** Only used before calibration. Calibration works out left and right by itself. */
    var swapLeftRight: Boolean
        get() = prefs.getBoolean("swap_left_right", false)
        set(v) = prefs.edit().putBoolean("swap_left_right", v).apply()

    // ---- Calibration ----

    /**
     * Calibration records one number - how far the eyes are turned sideways - while the
     * student looks at the middle, the left target and the right target. With the camera
     * at one end of the phone those three are rarely symmetrical, which is why it matters.
     */
    val calibration: Calibration?
        get() {
            if (!prefs.getBoolean("cal_done", false)) return null
            return Calibration(
                prefs.getFloat("cal_centre", 0f),
                prefs.getFloat("cal_left", 0f),
                prefs.getFloat("cal_right", 0f)
            ).takeIf { it.isValid }
        }

    val calibratedAtMs: Long get() = prefs.getLong("cal_at", 0L)

    fun saveCalibration(c: Calibration) {
        prefs.edit()
            .putBoolean("cal_done", true)
            .putFloat("cal_centre", c.centre)
            .putFloat("cal_left", c.left)
            .putFloat("cal_right", c.right)
            .putLong("cal_at", System.currentTimeMillis())
            .apply()
    }

    fun clearCalibration() {
        prefs.edit().remove("cal_done").remove("cal_at").apply()
    }

    // ---- Look ----

    var targetStyle: TargetStyle
        get() = runCatching { TargetStyle.valueOf(prefs.getString("target_style", null)!!) }
            .getOrDefault(TargetStyle.COLOURS)
        set(v) = prefs.edit().putString("target_style", v.name).apply()

    // ---- Outputs ----

    /** How long the relay stays closed for each switch press. */
    var pulseMs: Int
        get() = prefs.getInt("pulse_ms", DEFAULT_PULSE_MS)
        set(v) = prefs.edit().putInt("pulse_ms", v).apply()

    /** How long a song and lights reward lasts. */
    var rewardMs: Long
        get() = prefs.getLong("reward_ms", DEFAULT_REWARD_MS)
        set(v) = prefs.edit().putLong("reward_ms", v).apply()

    var soundEnabled: Boolean
        get() = prefs.getBoolean("sound_enabled", true)
        set(v) = prefs.edit().putBoolean("sound_enabled", v).apply()

    /**
     * The rear camera usually has a better lens and sensor than the front one, which
     * helps with a student wearing glasses, where lens glare is the usual problem.
     */
    var useBackCamera: Boolean
        get() = prefs.getBoolean("use_back_camera", false)
        set(v) = prefs.edit().putBoolean("use_back_camera", v).apply()

    // ---- Last session, shown on the home screen ----

    var lastSessionCount: Int
        get() = prefs.getInt("last_session_count", -1)
        set(v) = prefs.edit().putInt("last_session_count", v).apply()

    var lastSessionAtMs: Long
        get() = prefs.getLong("last_session_at", 0L)
        set(v) = prefs.edit().putLong("last_session_at", v).apply()

    fun resetToDefaults() {
        prefs.edit().clear().apply()
    }

    companion object {
        fun prefsName(profileId: String) =
            if (profileId == ProfileStore.DEFAULT_ID) "eye_switch_settings" else "eye_switch_settings_$profileId"

        const val DEFAULT_BLINK_THRESHOLD = 0.5f
        const val DEFAULT_HOLD_MS = 500L
        const val DEFAULT_REFRACTORY_MS = 900L
        const val DEFAULT_PULSE_MS = 250
        const val DEFAULT_REWARD_MS = 6000L

        const val MIN_HOLD_MS = 150
        const val MAX_HOLD_MS = 2000
        const val MIN_REFRACTORY_MS = 300
        const val MAX_REFRACTORY_MS = 3000
        const val MIN_PULSE_MS = 100
        const val MAX_PULSE_MS = 1000

        val REWARD_CHOICES_MS = longArrayOf(3000L, 6000L, 10000L)

        /**
         * Starting point before a teacher changes anything: looking right presses the
         * switch (the original goal), looking left plays a song with lights, and blink
         * is off, since staff at the classroom said gaze works better for these students.
         */
        fun defaultAction(g: Gesture): Action = when (g) {
            Gesture.LOOK_RIGHT -> Action.SWITCH_A
            Gesture.LOOK_LEFT -> Action.SONG_AND_LIGHTS
            Gesture.BLINK -> Action.NONE
        }
    }
}
