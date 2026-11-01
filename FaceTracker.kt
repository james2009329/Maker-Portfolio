package com.eyeswitch

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Matrix
import android.graphics.RectF
import android.hardware.display.DisplayManager
import android.os.SystemClock
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.framework.image.MPImage
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.core.Delegate
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.facelandmarker.FaceLandmarker
import com.google.mediapipe.tasks.vision.facelandmarker.FaceLandmarkerResult
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * What the camera saw in one frame.
 *
 * scores     - MediaPipe blendshape scores by name, or null if no face was found
 * faceBox    - where the face sits in the frame, 0-1 on each axis, or null
 * brightness - average brightness of the frame, 0 (black) to 1 (white)
 */
class FaceFrame(
    val scores: Map<String, Float>?,
    val faceBox: RectF?,
    val brightness: Float,
    val timeMs: Long
)

/**
 * Runs the camera and Google's MediaPipe Face Landmarker, entirely on the phone - no
 * video leaves the device. Shared by the student screen, the set-up screen and
 * calibration, so all three see the face the same way.
 *
 * The phone is used sideways and can be turned either way round, so the analyser is told
 * the screen's current rotation whenever it changes. That keeps the face upright in the
 * frames MediaPipe sees, which it needs to find the face at all.
 */
class FaceTracker(
    private val activity: AppCompatActivity,
    private val previewView: PreviewView?,
    private val useBackCamera: () -> Boolean,
    private val onFrame: (FaceFrame) -> Unit,
    private val onError: (Int) -> Unit
) : DefaultLifecycleObserver {

    private var landmarker: FaceLandmarker? = null
    private var provider: ProcessCameraProvider? = null
    private var preview: Preview? = null
    private var analysis: ImageAnalysis? = null
    private var boundBack: Boolean? = null
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()

    /** MediaPipe requires every frame's timestamp to be strictly increasing. */
    private var lastTs = 0L
    @Volatile private var lastBrightness = 0.5f

    private val displayManager =
        activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager

    /** Turning the phone 180 degrees does not restart the screen, so listen for it. */
    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayChanged(displayId: Int) {
            val rotation = currentRotation()
            analysis?.targetRotation = rotation
            preview?.targetRotation = rotation
        }
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
    }

    init {
        activity.lifecycle.addObserver(this)
    }

    fun start() {
        if (landmarker == null) landmarker = createLandmarker()
        val future = ProcessCameraProvider.getInstance(activity)
        future.addListener({
            provider = future.get()
            bind()
        }, ContextCompat.getMainExecutor(activity))
    }

    /** Rebinds if the teacher switched between front and rear camera. */
    fun rebindIfCameraChanged() {
        if (boundBack != null && boundBack != useBackCamera()) bind()
    }

    override fun onResume(owner: LifecycleOwner) {
        displayManager.registerDisplayListener(displayListener, null)
        // Another screen (e.g. calibration) may have taken the camera. Take it back.
        if (provider != null) bind()
    }

    override fun onPause(owner: LifecycleOwner) {
        displayManager.unregisterDisplayListener(displayListener)
    }

    override fun onDestroy(owner: LifecycleOwner) {
        executor.shutdown()
        landmarker?.close()
        landmarker = null
    }

    @Suppress("DEPRECATION")
    private fun currentRotation(): Int =
        (previewView?.display ?: activity.windowManager.defaultDisplay)?.rotation ?: 0

    private fun createLandmarker(): FaceLandmarker? = try {
        val base = BaseOptions.builder()
            .setModelAssetPath(MODEL_FILE)
            .setDelegate(Delegate.CPU)
            .build()
        val options = FaceLandmarker.FaceLandmarkerOptions.builder()
            .setBaseOptions(base)
            .setRunningMode(RunningMode.LIVE_STREAM)
            .setNumFaces(1)
            .setMinFaceDetectionConfidence(0.5f)
            .setMinFacePresenceConfidence(0.5f)
            .setMinTrackingConfidence(0.5f)
            // Blendshapes are the scores for "eye looking left", "eye blinking" and so on.
            .setOutputFaceBlendshapes(true)
            .setResultListener { result: FaceLandmarkerResult, input: MPImage -> onResult(result, input) }
            .setErrorListener { e: RuntimeException -> Log.e(TAG, "MediaPipe error", e) }
            .build()
        FaceLandmarker.createFromOptions(activity, options)
    } catch (e: Exception) {
        Log.e(TAG, "Could not create the face landmarker", e)
        onError(R.string.error_model)
        null
    }

    private fun bind() {
        val p = provider ?: return
        val wantBack = useBackCamera()

        var selector = if (wantBack) CameraSelector.DEFAULT_BACK_CAMERA else CameraSelector.DEFAULT_FRONT_CAMERA
        if (!p.hasCamera(selector)) {
            selector = if (wantBack) CameraSelector.DEFAULT_FRONT_CAMERA else CameraSelector.DEFAULT_BACK_CAMERA
            if (!p.hasCamera(selector)) {
                onError(R.string.error_camera)
                return
            }
        }

        val rotation = currentRotation()
        val newAnalysis = ImageAnalysis.Builder()
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_RGBA_8888)
            .setTargetRotation(rotation)
            .build()
            .also { it.setAnalyzer(executor, ::analyse) }

        val newPreview = previewView?.let { pv ->
            Preview.Builder().setTargetRotation(rotation).build().also { it.setSurfaceProvider(pv.surfaceProvider) }
        }

        try {
            p.unbindAll()
            if (newPreview != null) {
                p.bindToLifecycle(activity, selector, newPreview, newAnalysis)
            } else {
                p.bindToLifecycle(activity, selector, newAnalysis)
            }
            analysis = newAnalysis
            preview = newPreview
            boundBack = wantBack
        } catch (e: Exception) {
            Log.e(TAG, "Camera bind failed", e)
            onError(R.string.error_camera)
        }
    }

    /** Runs on the camera thread for every frame. */
    private fun analyse(imageProxy: ImageProxy) {
        val lm = landmarker
        if (lm == null) {
            imageProxy.close()
            return
        }

        val rotation = imageProxy.imageInfo.rotationDegrees
        val bitmap = try {
            imageProxy.toBitmap()
        } finally {
            // Must close on every path or the camera stalls after a few frames.
            imageProxy.close()
        }

        lastBrightness = brightnessOf(bitmap)

        val upright = if (rotation == 0) bitmap else {
            val m = Matrix().apply { postRotate(rotation.toFloat()) }
            Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, m, true)
        }

        var ts = SystemClock.uptimeMillis()
        if (ts <= lastTs) ts = lastTs + 1
        lastTs = ts

        try {
            lm.detectAsync(BitmapImageBuilder(upright).build(), ts)
        } catch (e: Exception) {
            Log.e(TAG, "detectAsync failed", e)
        }
    }

    /** MediaPipe calls this on its own thread. Hands a FaceFrame to the screen's UI thread. */
    private fun onResult(result: FaceLandmarkerResult, @Suppress("UNUSED_PARAMETER") input: MPImage) {
        val shapes = result.faceBlendshapes()
        val scores = if (shapes.isPresent && shapes.get().isNotEmpty()) {
            shapes.get()[0].associate { it.categoryName() to it.score() }
        } else null

        val box = result.faceLandmarks().firstOrNull()?.let { points ->
            var minX = 1f; var minY = 1f; var maxX = 0f; var maxY = 0f
            for (pt in points) {
                minX = minOf(minX, pt.x()); maxX = maxOf(maxX, pt.x())
                minY = minOf(minY, pt.y()); maxY = maxOf(maxY, pt.y())
            }
            RectF(minX, minY, maxX, maxY)
        }

        val frame = FaceFrame(scores, box, lastBrightness, SystemClock.uptimeMillis())
        activity.runOnUiThread { onFrame(frame) }
    }

    /** Average brightness from a coarse 16 x 16 grid - plenty to tell if a room is too dark. */
    private fun brightnessOf(b: Bitmap): Float {
        val n = 16
        var sum = 0f
        for (iy in 0 until n) for (ix in 0 until n) {
            val x = ((ix + 0.5f) * b.width / n).toInt().coerceIn(0, b.width - 1)
            val y = ((iy + 0.5f) * b.height / n).toInt().coerceIn(0, b.height - 1)
            val c = b.getPixel(x, y)
            val r = (c shr 16) and 0xFF
            val g = (c shr 8) and 0xFF
            val bl = c and 0xFF
            sum += (0.299f * r + 0.587f * g + 0.114f * bl) / 255f
        }
        return sum / (n * n)
    }

    companion object {
        private const val TAG = "FaceTracker"
        private const val MODEL_FILE = "face_landmarker.task"

        /** Below this the room is too dark for reliable eye tracking. */
        const val DARK_THRESHOLD = 0.18f
    }
}
