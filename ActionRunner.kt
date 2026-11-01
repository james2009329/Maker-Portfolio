package com.eyeswitch

import android.content.Context
import android.media.AudioManager
import android.media.ToneGenerator
import android.util.Log
import android.view.HapticFeedbackConstants
import android.view.View

/**
 * Carries out whatever the teacher assigned to a gesture: press the switch, play the
 * song, show the lights, or a mix. Used by student mode and by the test buttons on the
 * set-up screen, so a test does exactly what the real thing does.
 */
class ActionRunner(context: Context, lightsView: View) {

    private val app = EyeSwitchApp.of(context)
    private val reward = RewardPlayer(context)
    private val lights = Lights(lightsView)
    private val tone: ToneGenerator? = try {
        ToneGenerator(AudioManager.STREAM_MUSIC, 80)
    } catch (e: RuntimeException) {
        Log.w("ActionRunner", "No ToneGenerator", e)
        null
    }

    /** @param feel a view to buzz gently, so staff holding the phone feel each press. */
    fun perform(gesture: Gesture, settings: Settings, feel: View? = null) {
        val action = settings.actionFor(gesture)
        if (action == Action.NONE) return

        when (action) {
            Action.SWITCH_A -> app.usb.sendPulse('A', settings.pulseMs)
            Action.SWITCH_B -> app.usb.sendPulse('B', settings.pulseMs)
            else -> Unit
        }
        if (action.playsSong) reward.play(gesture, settings.songFor(gesture), settings.rewardMs)
        if (action.showsLights) lights.show(gesture, settings.rewardMs)

        // A short beep confirms a switch press. Skipped when a song is about to play anyway.
        if (settings.soundEnabled && !action.playsSong) {
            tone?.startTone(ToneGenerator.TONE_PROP_BEEP, 150)
        }
        feel?.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
    }

    fun stop() {
        reward.stop()
        lights.cancel()
    }

    fun release() {
        stop()
        tone?.release()
    }
}
