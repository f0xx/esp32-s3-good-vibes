package com.esp32s3.imusim

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import kotlin.math.max

/** Full-width top caption — FIFO queue, slide down/up between messages. */
class StatusBannerController private constructor(
    private val banner: TextView,
    private val handler: Handler,
) {
    private data class Item(
        val level: StatusBannerLevel,
        val message: String,
        val holdMs: Long,
        val coalesceKey: String?,
    )

    private val queue = ArrayDeque<Item>()
    private var hideRunnable: Runnable? = null
    private var visible = false
    private var draining = false
    private var current: Item? = null

    fun show(
        level: StatusBannerLevel,
        message: String,
        holdMs: Long = holdMsFor(level, message),
        coalesceKey: String? = coalesceKeyFor(message),
    ) {
        handler.post {
            val item = Item(level, message, holdMs, coalesceKey)
            if (coalesceKey != null) {
                val tail = queue.lastOrNull()
                if (tail?.coalesceKey == coalesceKey) {
                    queue[queue.lastIndex] = item
                    if (current?.coalesceKey == coalesceKey) {
                        applyItem(item)
                        rescheduleHide(item.holdMs)
                    }
                    return@post
                }
                if (current?.coalesceKey == coalesceKey) {
                    current = item
                    applyItem(item)
                    rescheduleHide(item.holdMs)
                    return@post
                }
            }
            queue.addLast(item)
            if (!draining) {
                drainNext()
            }
        }
    }

    fun hide() {
        handler.post {
            queue.clear()
            cancelHide()
            if (!visible) {
                draining = false
                current = null
                return@post
            }
            slideOut {
                visible = false
                current = null
                draining = false
            }
        }
    }

    private fun drainNext() {
        cancelHide()
        val next = queue.removeFirstOrNull()
        if (next == null) {
            if (visible) {
                draining = true
                slideOut {
                    visible = false
                    current = null
                    draining = false
                }
            } else {
                draining = false
                current = null
            }
            return
        }
        draining = true
        current = next
        applyItem(next)
        if (!visible) {
            slideIn()
            visible = true
        }
        scheduleHide(next.holdMs)
    }

    private fun applyItem(item: Item) {
        banner.text = item.message
        banner.setBackgroundColor(
            when (item.level) {
                StatusBannerLevel.OK -> banner.context.getColor(R.color.banner_ok)
                StatusBannerLevel.WARN -> banner.context.getColor(R.color.banner_warn)
                StatusBannerLevel.ERROR -> banner.context.getColor(R.color.banner_error)
            },
        )
    }

    private fun scheduleHide(holdMs: Long) {
        val task = Runnable {
            hideRunnable = null
            slideOut {
                visible = false
                current = null
                drainNext()
            }
        }
        hideRunnable = task
        handler.postDelayed(task, holdMs)
    }

    private fun rescheduleHide(holdMs: Long) {
        cancelHide()
        scheduleHide(holdMs)
    }

    private fun cancelHide() {
        hideRunnable?.let { handler.removeCallbacks(it) }
        hideRunnable = null
    }

    private fun slideIn() {
        banner.visibility = View.VISIBLE
        banner.alpha = 0f
        banner.post {
            val h = max(banner.height, banner.measuredHeight)
            banner.translationY = -h.toFloat()
            banner.animate().cancel()
            banner.animate()
                .translationY(0f)
                .alpha(1f)
                .setDuration(SLIDE_MS)
                .setListener(null)
                .start()
        }
    }

    private fun slideOut(onEnd: () -> Unit) {
        val h = max(banner.height, banner.measuredHeight)
        banner.animate().cancel()
        banner.animate()
            .translationY(-h.toFloat())
            .alpha(0f)
            .setDuration(SLIDE_MS)
            .setListener(object : AnimatorListenerAdapter() {
                override fun onAnimationEnd(animation: Animator) {
                    banner.visibility = View.GONE
                    onEnd()
                }
            })
            .start()
    }

    companion object {
        private const val SLIDE_MS = 280L

        fun attach(contentRoot: ViewGroup): StatusBannerController {
            val banner = android.view.LayoutInflater.from(contentRoot.context)
                .inflate(R.layout.view_status_banner, contentRoot, false) as TextView
            val lp = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP,
            )
            contentRoot.addView(banner, lp)
            ViewCompat.setOnApplyWindowInsetsListener(banner) { v, insets ->
                val top = insets.getInsets(WindowInsetsCompat.Type.statusBars()).top
                v.updatePadding(top = top + 10)
                insets
            }
            banner.visibility = View.GONE
            return StatusBannerController(banner, Handler(Looper.getMainLooper()))
        }

        /** ~5 s average: 3 s short, 5 s default, 7 s long / errors. */
        fun holdMsFor(level: StatusBannerLevel, message: String): Long {
            val len = message.length
            val base = when (level) {
                StatusBannerLevel.OK -> if (len <= 28) 3000L else 5000L
                StatusBannerLevel.WARN -> 5000L
                StatusBannerLevel.ERROR -> 7000L
            }
            return when {
                len >= 100 -> 7000L
                len >= 60 -> max(base, 6000L)
                len <= 16 && level == StatusBannerLevel.OK -> 3000L
                else -> base
            }
        }

        /** Progress-style captions share one slot (e.g. "Collecting samples (12/32)"). */
        fun coalesceKeyFor(message: String): String? {
            val paren = message.indexOf(" (")
            if (paren > 0) {
                return message.substring(0, paren)
            }
            return null
        }
    }
}
