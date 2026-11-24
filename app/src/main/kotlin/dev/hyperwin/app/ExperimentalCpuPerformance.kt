package dev.hyperwin.app

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

/** Explicit opt-in only. The Windows service stays demand-start, even when the preference is on. */
object ExperimentalCpuPerformance {
    const val KEY = "experimentalCpuPerformance"
    private const val TAG = "HyperWinCpu"
    private val work = Executors.newSingleThreadExecutor { r -> Thread(r, "hw-cpu-setting").apply { isDaemon = true } }
    private val revision = AtomicLong()
    private val main = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArraySet<(String) -> Unit>()
    @Volatile var status = "Off"
        private set
    @Volatile private var wanted = false

    fun enabled(ctx: Context) = ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).getBoolean(KEY, false)
    fun listen(listener: (String) -> Unit) { listeners.add(listener); listener(status) }
    fun unlisten(listener: (String) -> Unit) { listeners.remove(listener) }
    private fun publish(text: String) {
        status = text
        main.post { listeners.forEach { it(status) } }
    }
    fun setEnabled(ctx: Context, on: Boolean) {
        ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).edit().putBoolean(KEY, on).apply()
        sync(ctx)
    }
    fun disconnected() {
        revision.incrementAndGet()
        publish(if (wanted) "Waiting for Windows" else "Off")
    }
    fun sync(context: Context) {
        val ctx = context.applicationContext
        wanted = enabled(ctx)
        val on = wanted
        val ticket = revision.incrementAndGet()
        if (!GuestAgent.connected) { publish(if (on) "Will apply when Windows connects" else "Off"); return }
        publish(if (on) "Enabling…" else "Turning off…")
        work.execute {
            fun current() = ticket == revision.get() && GuestAgent.connected
            if (!current()) return@execute
            try {
                val script = ctx.assets.open("machine/guest/gpu/cpu-performance/toggle.ps1").bufferedReader().use { it.readText() }
                if (on) {
                    check(GuestAgent.push(ctx.assets.open("machine/guest/gpu/cpu-performance/hwcpufreq.sys"), "C:\\HyperWin\\hwcpufreq.next.sys"))
                    if (!current()) return@execute
                }
                if (!current()) return@execute
                val command = "& {\n$script\n}${if (on) " -Enable" else ""}"
                val encoded = android.util.Base64.encodeToString(command.toByteArray(Charsets.UTF_16LE), android.util.Base64.NO_WRAP)
                val done = CountDownLatch(1)
                var result = -1
                val id = GuestAgent.exec("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded",
                    onExit = { result = it; done.countDown() })
                val finished = done.await(45, TimeUnit.SECONDS)
                if (!finished) GuestAgent.forgetCompletion(id)
                if (!current()) return@execute
                if (finished && result == 0) publish(if (on) "Active (experimental)" else "Off")
                else publish(if (on) "Unavailable or failed to start — see Logs" else "Could not confirm stop — see Logs")
                Log.i(TAG, "CPU performance requested=$on exit=$result completed=$finished")
            } catch (e: Exception) {
                Log.w(TAG, "Could not apply CPU performance setting", e)
                if (current()) publish("Could not apply — see Logs")
            }
        }
    }
}
