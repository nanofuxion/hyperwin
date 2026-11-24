package dev.hyperwin.app

import android.view.Display
import android.view.Surface
import android.view.SurfaceControl
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
                makeLayer()
                startIfReady()
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                d3dLayer?.let { SurfaceControl.Transaction().setBufferSize(it, width, height).apply() }
                layer?.let { HwGpu.setLayer(it, d3dLayer, gameUnder, gameOver, pointer, width, height) }
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                dropLayer()
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

    /** GPU frames from Windows go here, a layer just above this view's own CPU-drawn picture. */
    private var layer: SurfaceControl? = null
    /** HwD3D's swap chains draw here (d3d/README.md), a layer above that one. */
    private var d3dLayer: SurfaceControl? = null
    /** The games renderer's layers (HwGpu.kt): a windowed game under the desktop, a full-screen one over it. */
    private var gameUnder: SurfaceControl? = null
    private var gameOver: SurfaceControl? = null
    /** Windows' mouse pointer, over everything (the display driver hands it over). */
    private var pointer: SurfaceControl? = null

    private fun container(name: String, z: Int) = SurfaceControl.Builder()
        .setName(name)
        .setParent(view.surfaceControl)
        .build()
        .also { SurfaceControl.Transaction().setLayer(it, z).setVisibility(it, true).apply() }

    private fun makeLayer() {
        if (layer != null) return
        val sc = SurfaceControl.Builder()
            .setName("hyperwin-scanout")
            .setParent(view.surfaceControl)
            .setOpaque(true)
            .setHidden(true)   // the renderer shows it with its first frame
            .build()
        SurfaceControl.Transaction().setLayer(sc, 2).apply {
            // Only a layer that gets frames counts toward the screen's rate, and during a game
            // that is this one: ask for the panel's fastest.
            bestMode(view.display)?.let {
                setFrameRate(sc, it.refreshRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT, Surface.CHANGE_FRAME_RATE_ALWAYS)
            }
        }.apply()
        layer = sc
        // A buffer layer the size of the view: a swap chain's frames are scaled to it.
        val d3d = SurfaceControl.Builder()
            .setName("hyperwin-d3d")
            .setParent(view.surfaceControl)
            .setBufferSize(view.width.coerceAtLeast(1), view.height.coerceAtLeast(1))
            .setOpaque(true)
            .setHidden(true)   // shown while a program has a swap chain
            .build()
        SurfaceControl.Transaction().setLayer(d3d, 3).apply()
        d3dLayer = d3d
        gameUnder = container("hyperwin-game-under", 1)
        gameOver = container("hyperwin-game-over", 4)
        pointer = SurfaceControl.Builder().setName("hyperwin-pointer").setParent(view.surfaceControl)
            .setHidden(true).build().also { SurfaceControl.Transaction().setLayer(it, 5).apply() }
        HwGpu.setLayer(sc, d3d, gameUnder, gameOver, pointer, view.width, view.height)
    }

    private fun dropLayer() {
        val sc = layer ?: return
        val others = listOfNotNull(d3dLayer, gameUnder, gameOver, pointer)
        layer = null
        d3dLayer = null
        gameUnder = null
        gameOver = null
        pointer = null
        HwGpu.setLayer(null, null, null, null, null, 0, 0)
        SurfaceControl.Transaction().reparent(sc, null).apply {
            others.forEach { reparent(it, null) }
        }.apply()
        sc.release()
        others.forEach { it.release() }
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
        HwGpu.hideLayer()
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
        // Each pass must fit in one refresh: Android hears so, and speeds the CPU up before one is late.
        val hint = GameSettings.Hint(view.context, 1_000_000_000L / 60)
        try {
            while (isCurrent(runGeneration, sharedWindow)) {
                if (passes++ % 120 == 0) {
                    refreshHz = view.display?.refreshRate?.takeIf { it >= 20f } ?: 60f
                    hint.target((1_000_000_000L / refreshHz).toLong())
                    hint.powerEfficient(GameSettings.capped(view.context))
                }
                val began = System.nanoTime()
                val presented = sharedWindow.withBuffer { WindowBridge.nativePresent(it) } ?: break
                hint.report(System.nanoTime() - began)
                if (presented == 1 && ++draws == 2) onLive()
                // One pass per screen refresh of the phone, less the time the pass took.
                nextFrame = maxOf(nextFrame + (1_000_000_000L / refreshHz).toLong(), System.nanoTime() + 1_000_000L)
                // Never negative: a late pass (the phone was busy) would make Thread.sleep throw.
                val wait = (nextFrame - System.nanoTime()).coerceAtLeast(0L)
                Thread.sleep(wait / 1_000_000L, (wait % 1_000_000L).toInt())
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        } finally {
            hint.close()
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
