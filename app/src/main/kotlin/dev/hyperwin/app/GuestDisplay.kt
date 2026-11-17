package dev.hyperwin.app

import android.view.Display
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import java.util.concurrent.atomic.AtomicLong

/**
 * Presents the AVF-backed guest frame on the app's own SurfaceView. The surface buffer has the
 * guest's size and the phone's compositor scales it, so the view itself is kept at the guest's shape.
 */
class GuestDisplay(
    private val view: SurfaceView,
    /** Called once per start, when the guest first changes the picture after its initial frame. */
    private val onLive: () -> Unit,
) {
    private val lock = Any()
    private val generation = AtomicLong(0)
    @Volatile private var window: AvfPlan.SharedWindow? = null
    @Volatile private var running = false
    @Volatile private var renderThread: Thread? = null

    init {
        view.setSurfaceLifecycle(SurfaceView.SURFACE_LIFECYCLE_FOLLOWS_ATTACHMENT)
        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                // Ask for the panel's fastest rate; the render loop keeps the screen's actual pace.
                bestMode(view.display)?.let {
                    runCatching { holder.surface.setFrameRate(it.refreshRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT) }
                }
                WindowBridge.nativeSetSurface(holder.surface)
                startIfReady()
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                WindowBridge.nativeSetSurface(null)   // returns once no draw is in flight
                stop()
            }
        })
        view.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) {
                (v.parent as? View)?.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> fit() }
            }
            override fun onViewDetachedFromWindow(v: View) = Unit
        })
    }

    fun vmStarted(sharedWindow: AvfPlan.SharedWindow?) {
        synchronized(lock) {
            window = sharedWindow
            running = true
            generation.incrementAndGet()
        }
        refit()
        if (sharedWindow != null) startIfReady()
    }

    fun vmStopped() {
        val thread: Thread?
        synchronized(lock) {
            running = false
            generation.incrementAndGet()
            thread = renderThread
            thread?.interrupt()
        }
        if (thread != null && thread !== Thread.currentThread()) runCatching { thread.join(250) }
        window = null
    }

    fun saveFrame() = Unit

    fun refit() { view.post { fit() } }

    /** Sizes the view to the largest rectangle of the guest's shape that fits its parent, centred. */
    private fun fit() {
        val parent = view.parent as? View ?: return
        val roomW = parent.width - parent.paddingLeft - parent.paddingRight
        val roomH = parent.height - parent.paddingTop - parent.paddingBottom
        if (roomW <= 0 || roomH <= 0) return
        val scale = minOf(roomW.toFloat() / AvfPlan.frameWidth, roomH.toFloat() / AvfPlan.frameHeight)
        val w = (AvfPlan.frameWidth * scale).toInt()
        val h = (AvfPlan.frameHeight * scale).toInt()
        val params = view.layoutParams as? android.widget.FrameLayout.LayoutParams ?: return
        if (params.width == w && params.height == h) return
        params.width = w
        params.height = h
        params.gravity = android.view.Gravity.CENTER
        view.layoutParams = params
    }

    private fun stop() {
        synchronized(lock) {
            generation.incrementAndGet()
            renderThread?.interrupt()
        }
    }

    private fun startIfReady() {
        val sharedWindow = window ?: return
        if (!running || !view.holder.surface.isValid) return
        val current = generation.get()
        synchronized(lock) {
            if (generation.get() != current || !running || window !== sharedWindow) return
            generation.incrementAndGet()
        }
        val runGeneration = generation.get()
        Thread({ render(sharedWindow, runGeneration) }, "hyperwin-frame-ring")
            .apply {
                isDaemon = true
                renderThread = this
                start()
            }
    }

    private fun render(sharedWindow: AvfPlan.SharedWindow, runGeneration: Long) {
        var draws = 0
        var passes = 0
        var refreshHz = 60f
        var nextFrame = System.nanoTime()
        try {
            while (isCurrent(runGeneration, sharedWindow)) {
                if (passes++ % 120 == 0) refreshHz = view.display?.refreshRate?.takeIf { it >= 20f } ?: 60f
                if (WindowBridge.nativePresent(sharedWindow.buffer) == 1 && ++draws == 2) onLive()
                // One pass per screen refresh of the phone, less the time the pass took.
                nextFrame = maxOf(nextFrame + (1_000_000_000L / refreshHz).toLong(), System.nanoTime() + 1_000_000L)
                // Never negative: a late pass (the phone was busy) would make Thread.sleep throw.
                val wait = (nextFrame - System.nanoTime()).coerceAtLeast(0L)
                Thread.sleep(wait / 1_000_000L, (wait % 1_000_000L).toInt())
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        } finally {
            if (renderThread === Thread.currentThread()) renderThread = null
        }
    }

    private fun isCurrent(expected: Long, sharedWindow: AvfPlan.SharedWindow): Boolean =
        running && generation.get() == expected && window === sharedWindow

    companion object {
        /** The fastest refresh rate the screen offers at its current resolution. */
        fun bestMode(display: Display?): Display.Mode? {
            val now = display?.mode ?: return null
            return display.supportedModes
                .filter { it.physicalWidth == now.physicalWidth && it.physicalHeight == now.physicalHeight }
                .maxByOrNull { it.refreshRate }
        }
    }
}
