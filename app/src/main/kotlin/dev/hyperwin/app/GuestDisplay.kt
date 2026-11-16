package dev.hyperwin.app

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicLong

/** Presents the AVF-backed guest frame ring on the app's own SurfaceView. */
class GuestDisplay(
    private val view: SurfaceView,
    private val onStatus: (String) -> Unit,
) {
    private val lock = Any()
    private val generation = AtomicLong(0)
    @Volatile private var window: AvfPlan.SharedWindow? = null
    @Volatile private var running = false
    @Volatile private var renderThread: Thread? = null

    init {
        view.setSurfaceLifecycle(SurfaceView.SURFACE_LIFECYCLE_FOLLOWS_ATTACHMENT)
        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = startIfReady()
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
            override fun surfaceDestroyed(holder: SurfaceHolder) = stop()
        })
    }

    fun vmStarted(sharedWindow: AvfPlan.SharedWindow?) {
        synchronized(lock) {
            window = sharedWindow
            running = true
            generation.incrementAndGet()
        }
        if (sharedWindow == null) {
            onStatus("AVF shared framebuffer is not attached.")
        } else {
            startIfReady()
        }
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
        val frame = ByteBuffer.allocateDirect(AvfPlan.FRAME_BYTES)
        val bitmap = Bitmap.createBitmap(AvfPlan.FRAME_WIDTH, AvfPlan.FRAME_HEIGHT, Bitmap.Config.ARGB_8888)
        val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        var lastSequence = 0
        var reportedFrame = false
        val startTime = System.nanoTime()
        try {
            while (isCurrent(runGeneration, sharedWindow)) {
                if (!view.holder.surface.isValid) {
                    Thread.sleep(25)
                    continue
                }
                frame.clear()
                val sequence = WindowBridge.nativeCopyFrame(sharedWindow.buffer, frame)
                val gopFramebuffer = sequence == -2
                if (gopFramebuffer || (sequence > 0 && sequence != lastSequence)) {
                    frame.position(0)
                    bitmap.copyPixelsFromBuffer(frame)
                    draw(bitmap, paint)
                    if (!gopFramebuffer) lastSequence = sequence
                    if (!reportedFrame) {
                        reportedFrame = true
                        onStatus(if (gopFramebuffer) "UEFI GOP framebuffer is live." else "Shared framebuffer connected; guest sequence $sequence.")
                    }
                } else if (!reportedFrame && System.nanoTime() - startTime > 5_000_000_000L) {
                    reportedFrame = true
                    onStatus("Waiting for U-Boot/EFI or Windows to publish a frame.")
                }
                Thread.sleep(16)
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        } finally {
            bitmap.recycle()
            if (renderThread === Thread.currentThread()) renderThread = null
        }
    }

    private fun draw(bitmap: Bitmap, paint: Paint) {
        val holder = view.holder
        val canvas = runCatching { holder.lockCanvas() }.getOrNull() ?: return
        try {
            canvas.drawColor(0xff000000.toInt())
            val dst = fitRect(canvas, bitmap.width, bitmap.height)
            canvas.drawBitmap(bitmap, null, dst, paint)
        } finally {
            holder.unlockCanvasAndPost(canvas)
        }
    }

    private fun fitRect(canvas: Canvas, width: Int, height: Int): android.graphics.RectF {
        val scale = minOf(canvas.width.toFloat() / width, canvas.height.toFloat() / height)
        val w = width * scale
        val h = height * scale
        val left = (canvas.width - w) / 2f
        val top = (canvas.height - h) / 2f
        return android.graphics.RectF(left, top, left + w, top + h)
    }

    private fun isCurrent(expected: Long, sharedWindow: AvfPlan.SharedWindow): Boolean =
        running && generation.get() == expected && window === sharedWindow
}
