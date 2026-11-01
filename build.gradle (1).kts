plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.eyeswitch"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.eyeswitch"
        minSdk = 26
        targetSdk = 34
        versionCode = 3
        versionName = "3.0"

        // MediaPipe ships native code; these two cover every modern Android phone
        // and keep the APK much smaller than bundling x86 too.
        ndk {
            abiFilters.addAll(listOf("arm64-v8a", "armeabi-v7a"))
        }
    }

    // MediaPipe memory-maps the model, which only works if it is stored uncompressed.
    androidResources {
        noCompress.add("task")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.activity:activity-ktx:1.9.2")

    // Material 3 components (cards, chips, sliders, switches), the start-up splash
    // screen, and the swipeable walkthrough.
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.core:core-splashscreen:1.0.1")
    implementation("androidx.viewpager2:viewpager2:1.1.0")

    // CameraX - preview and frame analysis
    val camerax = "1.3.4"
    implementation("androidx.camera:camera-core:$camerax")
    implementation("androidx.camera:camera-camera2:$camerax")
    implementation("androidx.camera:camera-lifecycle:$camerax")
    implementation("androidx.camera:camera-view:$camerax")

    // MediaPipe Face Landmarker - finds the face and irises and reports where the eyes
    // are looking. Runs entirely on the phone, no internet needed once installed.
    implementation("com.google.mediapipe:tasks-vision:0.10.14")

    // USB serial to the ESP32
    implementation("com.github.mik3y:usb-serial-for-android:3.7.0")
}

// The face model is about 3.6 MB and is not kept in the source folder. Gradle fetches it
// once, the first time the project builds, and skips this step after that.
val faceModel = file("src/main/assets/face_landmarker.task")
val downloadFaceModel by tasks.registering {
    outputs.file(faceModel)
    onlyIf { !faceModel.exists() }
    doLast {
        faceModel.parentFile.mkdirs()
        // uri() rather than java.net.URL: inside a Gradle script "java" means the Java plugin.
        val url = uri(
            "https://storage.googleapis.com/mediapipe-models/face_landmarker/" +
                "face_landmarker/float16/1/face_landmarker.task"
        ).toURL()
        url.openStream().use { input -> faceModel.outputStream().use { input.copyTo(it) } }
    }
}
tasks.named("preBuild") { dependsOn(downloadFaceModel) }
