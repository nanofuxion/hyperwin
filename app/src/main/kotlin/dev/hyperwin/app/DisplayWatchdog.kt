package dev.hyperwin.app

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import java.io.File

/**
 * Windows sometimes comes up with a black screen: its screen driver is attached but never gets a
 * swap chain, so nothing reaches the phone (wddm/README.md, "commit modes: 0 paths"). A while after
 * the agent answers, if the GPU screen was asked for and no frame has arrived, switch Windows'
 * screen driver to the software path (frames read back by the CPU) and say so. The next start tries
 * the GPU path again.
 */
object DisplayWatchdog {
    private const val WAIT_MS = 45_000L
    private const val KEY = "HKLM\\SOFTWARE\\HyperWin\\Gpu"
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var startedAt = 0L
    @Volatile private var armed = false
    private var check: Runnable? = null

    fun flagFile(ctx: Context) = File(ctx.cacheDir, "screen-live")

    /** A Windows start: the clock for "a frame arrived since" begins here. */
    fun vmStarted() {
        startedAt = System.currentTimeMillis()
        armed = false
        check?.let { main.removeCallbacks(it) }
    }

    /** The agent in Windows answered. Called on every hello; only the first after a start counts. */
    fun agentUp(ctx: Context) {
        if (armed) return
        armed = true
        val app = ctx.applicationContext
        val r = Runnable { ask(app) }
        check = r
        main.postDelayed(r, WAIT_MS)
    }

    /** What the watchdog decided, kept in cache/display-watchdog.log (logcat is cleared by the test scripts). */
    private fun note(ctx: Context, text: String) {
        android.util.Log.i("HwDisplay", text)
        runCatching { File(ctx.cacheDir, "display-watchdog.log").appendText("${System.currentTimeMillis() / 1000} $text\n") }
    }

    private fun shown(ctx: Context) = flagFile(ctx).lastModified() >= startedAt

    private fun ask(ctx: Context) {
        note(ctx, "check: gpuFrames=${shown(ctx)} connected=${GuestAgent.connected}")
        if (shown(ctx) || !GuestAgent.connected) return
        // The screen may be on the plain path with no GPU frames: look at what is on the phone.
        onBlack(ctx) {
            // Exit 0 when the GPU screen was asked for (IddRender 1); without it black is not this problem.
            GuestAgent.onceDone(GuestAgent.exec("cmd /c reg query $KEY /v IddRender | find \"0x1\"")) { exit ->
                note(ctx, "IddRender query exit $exit")
                if (exit == 0) failOver(ctx)
            }
        }
    }

    /** Runs [then] when the guest frame is all but black. No frame at all counts as black too. */
    private fun onBlack(ctx: Context, then: () -> Unit) {
        val w = AvfLaunch.sharedWindow
        val peak = w?.withBuffer { WindowBridge.nativeFramePeak(it) } ?: -1
        note(ctx, "brightest sample in the guest frame: $peak")
        if (peak < 40) then()
    }

    private fun failOver(ctx: Context) {
        note(ctx, "FALLBACK: black screen after ${(System.currentTimeMillis() - startedAt) / 1000}s; capturing display-driver state and switching to software display")
        GuestAgent.exec("cmd /c echo === FALLBACK %DATE% %TIME% === >> C:\\ProgramData\\HyperWin\\watchdog-fallback.log & " +
            "C:\\HyperWin\\gpu\\dispinfo.exe >> C:\\ProgramData\\HyperWin\\watchdog-fallback.log 2>&1 & " +
            "powershell -NoProfile -Command \"Get-Content C:\\ProgramData\\HyperWin\\idd.log -Tail 25\" >> C:\\ProgramData\\HyperWin\\watchdog-fallback.log 2>&1 & " +
            "reg add $KEY /v IddRender /t REG_DWORD /d 0 /f & pnputil /restart-device ROOT\\DISPLAY\\0000 & " +
            // Only this start runs on the software screen; the next one tries the GPU again.
            "ping -n 21 127.0.0.1 >nul & reg add $KEY /v IddRender /t REG_DWORD /d 1 /f")
        main.post { Toast.makeText(ctx, "The display didn't load. Switched to the software display.", Toast.LENGTH_LONG).show() }
    }
}
