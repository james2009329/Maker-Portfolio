package com.eyeswitch

import android.content.Intent
import android.os.Bundle
import android.text.format.DateUtils
import android.view.LayoutInflater
import android.view.View
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText

/**
 * Home. Three big choices - Start, Set up, Settings - and which student is selected.
 * Kept deliberately simple so a busy aide can get a student going in two taps.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var profiles: ProfileStore
    private lateinit var profileButton: MaterialButton
    private lateinit var calibrationTag: TextView
    private lateinit var lastSessionText: TextView
    private lateinit var statusPill: View

    private val usbListener: (UsbSerialManager.Status) -> Unit = { UiKit.renderStatus(statusPill, it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        profiles = ProfileStore(this)
        profileButton = findViewById(R.id.profileButton)
        calibrationTag = findViewById(R.id.calibrationTag)
        lastSessionText = findViewById(R.id.lastSessionText)
        statusPill = findViewById(R.id.statusPill)

        findViewById<View>(R.id.startCard).setOnClickListener {
            UiKit.tick(it)
            startActivity(Intent(this, StudentActivity::class.java))
        }
        findViewById<View>(R.id.setupCard).setOnClickListener {
            UiKit.tick(it)
            startActivity(Intent(this, SetupActivity::class.java))
        }
        findViewById<View>(R.id.settingsCard).setOnClickListener {
            UiKit.tick(it)
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<View>(R.id.helpButton).setOnClickListener {
            startActivity(Intent(this, OnboardingActivity::class.java))
        }
        profileButton.setOnClickListener { showProfiles() }

        if (!profiles.onboardingSeen && savedInstanceState == null) {
            startActivity(Intent(this, OnboardingActivity::class.java))
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
        val app = EyeSwitchApp.of(this)
        app.addUsbListener(usbListener)
        app.usb.connect()
    }

    override fun onPause() {
        super.onPause()
        EyeSwitchApp.of(this).removeUsbListener(usbListener)
    }

    private fun refresh() {
        val settings = Settings(this)
        profileButton.text = profiles.currentName
        UiKit.renderCalibration(calibrationTag, settings)

        val count = settings.lastSessionCount
        lastSessionText.text = if (count >= 0) {
            getString(
                R.string.home_last_session,
                resources.getQuantityString(R.plurals.activations, count, count),
                DateUtils.getRelativeTimeSpanString(
                    settings.lastSessionAtMs, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS
                )
            )
        } else ""
    }

    // ---------------------------------------------------------------- students

    private fun showProfiles() {
        val ids = profiles.ids
        val names = ids.map { profiles.nameOf(it) }.toTypedArray()
        val current = ids.indexOf(profiles.currentId)

        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.profile_title)
            .setSingleChoiceItems(names, current) { d, which ->
                profiles.currentId = ids[which]
                UiKit.tick(profileButton)
                refresh()
                d.dismiss()
            }
            .setPositiveButton(R.string.profile_add) { _, _ -> askName(null) }
            .setNeutralButton(R.string.profile_rename) { _, _ -> askName(profiles.currentId) }
            .setNegativeButton(R.string.profile_remove) { _, _ -> confirmRemove() }
            .show()
    }

    /** Asks for a name. With an id it renames that student, otherwise it adds a new one. */
    private fun askName(renameId: String?) {
        val view = LayoutInflater.from(this).inflate(R.layout.dialog_name, null)
        val input = view.findViewById<TextInputEditText>(R.id.nameInput)
        if (renameId != null) input.setText(profiles.nameOf(renameId))

        MaterialAlertDialogBuilder(this)
            .setTitle(if (renameId == null) R.string.profile_add else R.string.profile_rename)
            .setView(view)
            .setPositiveButton(R.string.ok) { _, _ ->
                val name = input.text?.toString()?.trim().orEmpty()
                if (name.isEmpty()) return@setPositiveButton
                if (renameId == null) profiles.add(name) else profiles.rename(renameId, name)
                refresh()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
        input.requestFocus()
    }

    private fun confirmRemove() {
        if (profiles.ids.size <= 1) {
            MaterialAlertDialogBuilder(this)
                .setMessage(R.string.profile_cannot_remove)
                .setPositiveButton(R.string.ok, null)
                .show()
            return
        }
        val id = profiles.currentId
        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.profile_remove_title, profiles.nameOf(id)))
            .setMessage(R.string.profile_remove_body)
            .setPositiveButton(R.string.profile_remove) { _, _ ->
                profiles.delete(id)
                refresh()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }
}
