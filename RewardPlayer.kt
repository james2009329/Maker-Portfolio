package com.eyeswitch

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaPlayer
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * Plays the sound reward for a gesture: a song the teacher chose from the phone, or a
 * short built-in tune if none was chosen (or the chosen file won't play).
 *
 * Call everything from the main thread.
 */
class RewardPlayer(private val context: Context) {

    private val handler = Handler(Looper.getMainLooper())
    private var mediaPlayer: MediaPlayer? = null
    private var track: AudioTrack? = null
    private var fadeVolume = 1f

    private val stopRunnable = Runnable { stop() }

    /** Lowers the volume in ten steps so a song fades out instead of cutting off. */
    private val fadeStep = object : Runnable {
        override fun run() {
            val mp = mediaPlayer ?: return
            fadeVolume -= 0.1f
            if (fadeVolume <= 0f) {
                stop()
                return
            }
            runCatching { mp.setVolume(fadeVolume, fadeVolume) }
            handler.postDelayed(this, FADE_MS / 10)
        }
    }

    fun play(gesture: Gesture, songUri: Uri?, lengthMs: Long) {
        stop()
        if (songUri != null && playFile(songUri)) {
            fadeVolume = 1f
            handler.postDelayed(fadeStep, (lengthMs - FADE_MS).coerceAtLeast(0L))
            handler.postDelayed(stopRunnable, lengthMs + 200)
        } else {
            playBuiltIn(gesture, lengthMs)
            handler.postDelayed(stopRunnable, lengthMs + 200)
        }
    }

    private fun playFile(uri: Uri): Boolean {
        return try {
            val mp = MediaPlayer()
            mp.setAudioAttributes(ATTRS)
            mp.setDataSource(context, uri)
            mp.setOnPreparedListener { it.start() }
            mp.setOnErrorListener { _, _, _ ->
                this@RewardPlayer.stop()
                true
            }
            mp.prepareAsync()
            mediaPlayer = mp
            true
        } catch (e: Exception) {
            Log.w(TAG, "Could not play the chosen song, using the built-in tune", e)
            mediaPlayer?.release()
            mediaPlayer = null
            false
        }
    }

    private fun playBuiltIn(gesture: Gesture, lengthMs: Long) {
        try {
            val samples = Jingle.render(gesture, lengthMs, SAMPLE_RATE)
            val t = AudioTrack.Builder()
                .setAudioAttributes(ATTRS)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                .setTransferMode(AudioTrack.MODE_STATIC)
                .setBufferSizeInBytes(samples.size * 2)
                .build()
            t.write(samples, 0, samples.size)
            t.play()
            track = t
        } catch (e: Exception) {
            Log.e(TAG, "Could not play the built-in tune", e)
        }
    }

    fun stop() {
        handler.removeCallbacks(stopRunnable)
        handler.removeCallbacks(fadeStep)
        mediaPlayer?.let { mp ->
            runCatching { if (mp.isPlaying) mp.stop() }
            mp.release()
        }
        mediaPlayer = null
        track?.let { t ->
            runCatching { t.stop() }
            t.release()
        }
        track = null
    }

    companion object {
        private const val TAG = "RewardPlayer"
        private const val SAMPLE_RATE = 22050
        private const val FADE_MS = 1000L

        private val ATTRS: AudioAttributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
    }
}

/**
 * Generates a short, cheerful tune for each gesture, so a student can learn that
 * "looking left sounds like this". The melodies are original, built from the
 * pentatonic scale so any combination of notes sounds pleasant.
 */
object Jingle {

    private val melodies = mapOf(
        Gesture.LOOK_LEFT to intArrayOf(72, 76, 79, 81, 79, 76),
        Gesture.LOOK_RIGHT to intArrayOf(67, 69, 72, 74, 76, 74, 72, 69),
        Gesture.BLINK to intArrayOf(72, 67, 69, 76, 74, 79)
    )

    private const val NOTE_MS = 220
    private const val REST_MS = 440

    fun render(gesture: Gesture, lengthMs: Long, rate: Int): ShortArray {
        val total = (rate.toLong() * lengthMs / 1000L).toInt()
        val out = ShortArray(total)
        val notes = melodies.getValue(gesture)
        val noteLen = rate * NOTE_MS / 1000
        val restLen = rate * REST_MS / 1000

        var pos = 0
        outer@ while (pos < total) {
            for (midi in notes) {
                val freq = 440.0 * Math.pow(2.0, (midi - 69) / 12.0)
                for (i in 0 until noteLen) {
                    if (pos >= total) break@outer
                    val t = i.toDouble() / rate
                    // A sine plus a quieter octave above sounds softer than a pure beep.
                    val wave = (Math.sin(2 * Math.PI * freq * t) + 0.3 * Math.sin(4 * Math.PI * freq * t)) / 1.3
                    out[pos] = (wave * envelope(i, noteLen) * 0.5 * Short.MAX_VALUE).toInt().toShort()
                    pos++
                }
            }
            pos += restLen
        }

        // Fade the last half second so the tune never cuts off abruptly.
        val fade = minOf(total, rate / 2)
        for (i in 0 until fade) {
            val idx = total - fade + i
            out[idx] = (out[idx] * (1f - i.toFloat() / fade)).toInt().toShort()
        }
        return out
    }

    /** Quick attack, then a gentle decay - like a plucked note rather than a buzzer. */
    private fun envelope(i: Int, len: Int): Double {
        val attack = maxOf(1, len / 20)
        return if (i < attack) {
            i.toDouble() / attack
        } else {
            1.0 - (i - attack).toDouble() / (len - attack) * 0.8
        }
    }
}
