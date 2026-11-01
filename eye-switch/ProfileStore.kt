package com.eyeswitch

import android.content.Context

/**
 * The list of students. Each one has a name and its own Settings file, so switching
 * student brings back their calibration, actions and target style in one tap.
 */
class ProfileStore(private val context: Context) {

    private val prefs = context.getSharedPreferences("eye_switch_profiles", Context.MODE_PRIVATE)

    val ids: List<String>
        get() = prefs.getString("ids", null)
            ?.split(',')
            ?.filter { it.isNotBlank() }
            ?.takeIf { it.isNotEmpty() }
            ?: listOf(DEFAULT_ID)

    var currentId: String
        get() = prefs.getString("current", null)?.takeIf { it in ids } ?: ids.first()
        set(v) = prefs.edit().putString("current", v).apply()

    fun nameOf(id: String): String =
        prefs.getString("name_$id", null)
            ?: context.getString(R.string.profile_default_name, ids.indexOf(id) + 1)

    val currentName: String get() = nameOf(currentId)

    /** Adds a student and makes them the current one. */
    fun add(name: String): String {
        val id = "p${System.currentTimeMillis()}"
        prefs.edit()
            .putString("ids", (ids + id).joinToString(","))
            .putString("name_$id", name.trim())
            .putString("current", id)
            .apply()
        return id
    }

    fun rename(id: String, name: String) {
        prefs.edit().putString("name_$id", name.trim()).apply()
    }

    /** Removes a student and their settings. The last remaining student cannot be removed. */
    fun delete(id: String): Boolean {
        val remaining = ids - id
        if (remaining.isEmpty()) return false
        prefs.edit()
            .putString("ids", remaining.joinToString(","))
            .remove("name_$id")
            .putString("current", remaining.first())
            .apply()
        context.deleteSharedPreferences(Settings.prefsName(id))
        return true
    }

    var onboardingSeen: Boolean
        get() = prefs.getBoolean("onboarding_seen", false)
        set(v) = prefs.edit().putBoolean("onboarding_seen", v).apply()

    companion object {
        const val DEFAULT_ID = "default"
    }
}
