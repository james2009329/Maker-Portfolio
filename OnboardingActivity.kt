package com.eyeswitch

import android.content.res.ColorStateList
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.RecyclerView
import androidx.viewpager2.widget.ViewPager2
import com.google.android.material.button.MaterialButton

/**
 * The first-run walkthrough: three cards a teacher swipes through, covering mounting
 * the phone, plugging in the toy and calibrating. Reachable again from "How to set up".
 */
class OnboardingActivity : AppCompatActivity() {

    private class Page(val icon: Int, val colour: Int, val title: Int, val body: Int)

    private val pages = listOf(
        Page(R.drawable.ic_landscape, Palette.of(Gesture.LOOK_LEFT), R.string.onboard_1_title, R.string.onboard_1_body),
        Page(R.drawable.ic_plug, Palette.of(Gesture.LOOK_RIGHT), R.string.onboard_2_title, R.string.onboard_2_body),
        Page(R.drawable.ic_target, Palette.of(Gesture.BLINK), R.string.onboard_3_title, R.string.onboard_3_body)
    )

    private lateinit var pager: ViewPager2
    private lateinit var next: MaterialButton
    private lateinit var dots: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_onboarding)

        pager = findViewById(R.id.pager)
        next = findViewById(R.id.nextButton)
        dots = findViewById(R.id.dots)

        pager.adapter = Adapter()
        buildDots()
        pager.registerOnPageChangeCallback(object : ViewPager2.OnPageChangeCallback() {
            override fun onPageSelected(position: Int) {
                updateDots(position)
                next.setText(if (position == pages.lastIndex) R.string.onboard_done else R.string.onboard_next)
            }
        })

        next.setOnClickListener {
            UiKit.tick(it)
            if (pager.currentItem < pages.lastIndex) pager.currentItem += 1 else done()
        }
        findViewById<View>(R.id.skipButton).setOnClickListener { done() }
    }

    private fun done() {
        ProfileStore(this).onboardingSeen = true
        finish()
    }

    private fun buildDots() {
        val size = (10 * resources.displayMetrics.density).toInt()
        val gap = (6 * resources.displayMetrics.density).toInt()
        repeat(pages.size) {
            val dot = View(this).apply {
                setBackgroundResource(R.drawable.bg_dot)
                layoutParams = LinearLayout.LayoutParams(size, size).apply { marginStart = gap; marginEnd = gap }
            }
            dots.addView(dot)
        }
        updateDots(0)
    }

    private fun updateDots(selected: Int) {
        for (i in 0 until dots.childCount) {
            val dot = dots.getChildAt(i)
            dot.backgroundTintList = ColorStateList.valueOf(if (i == selected) Palette.TEXT else Palette.SURFACE_RAISED)
            dot.animate().scaleX(if (i == selected) 1.3f else 1f).scaleY(if (i == selected) 1.3f else 1f)
                .setDuration(200).start()
        }
    }

    private inner class Adapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        override fun getItemCount() = pages.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder =
            object : RecyclerView.ViewHolder(
                LayoutInflater.from(parent.context).inflate(R.layout.item_onboarding_page, parent, false)
            ) {}

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            val p = pages[position]
            val v = holder.itemView
            v.findViewById<ImageView>(R.id.pageIcon).apply {
                setImageResource(p.icon)
                imageTintList = ColorStateList.valueOf(p.colour)
            }
            v.findViewById<TextView>(R.id.pageStep).text =
                getString(R.string.onboard_step, position + 1, pages.size)
            v.findViewById<TextView>(R.id.pageTitle).setText(p.title)
            v.findViewById<TextView>(R.id.pageBody).setText(p.body)
        }
    }
}
