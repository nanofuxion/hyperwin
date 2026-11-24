package dev.hyperwin.app

import android.app.GameManager
import android.app.GameState
import android.content.Context
import android.os.PerformanceHintManager
import android.os.PowerManager
import android.os.Process
import android.util.Log
import android.view.Display

/**
 * Android's game settings (game-mode/README.md): the Game Dashboard's mode, the loading state while
 * Windows boots, the phone's heat, and a performance hint for the frame thread.
 */
object GameSettings {
    private const val TAG = "HyperWinGame"
    const val KEEP_COOL = "keepCool"

    @Volatile var hot = false
        private set
    /** Called on the main thread when [hot] or the Game Dashboard mode may change what is capped. */
    var onChange: (() -> Unit)? = null

    private var power: PowerManager? = null
    private fun prefs(ctx: Context) = ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE)
    private val listener = PowerManager.OnThermalStatusChangedListener { status -> thermal(status) }

    /** Performance, battery, standard or unsupported, from the Game Dashboard. */
    fun mode(ctx: Context): Int =
        runCatching { ctx.getSystemService(GameManager::class.java).gameMode }.getOrDefault(GameManager.GAME_MODE_UNSUPPORTED)

    /** True when frames should stay at 60: the Battery game mode, or a hot phone with "Keep the phone cool". */
    fun capped(ctx: Context): Boolean =
        mode(ctx) == GameManager.GAME_MODE_BATTERY || (hot && prefs(ctx).getBoolean(KEEP_COOL, true))

    /** The display mode to ask for: the fastest, or a 60 Hz one at the same size while [capped]. */
    fun displayMode(ctx: Context, display: Display?): Display.Mode? {
        val best = GuestDisplay.bestMode(display) ?: return null
        if (!capped(ctx)) return best
        return display?.supportedModes
            ?.filter { it.physicalWidth == best.physicalWidth && it.physicalHeight == best.physicalHeight && it.refreshRate >= 59f }
            ?.minByOrNull { it.refreshRate } ?: best
    }

    /** Android may boost the phone while a game loads: Windows booting counts. */
    fun loading(ctx: Context, on: Boolean) {
        runCatching {
            ctx.getSystemService(GameManager::class.java)
                .setGameState(GameState(on, if (on) GameState.MODE_NONE else GameState.MODE_GAMEPLAY_INTERRUPTIBLE))
        }.onFailure { Log.w(TAG, "game state not taken", it) }
    }

    /** Starts listening to the phone's heat while Windows runs. */
    fun watchHeat(ctx: Context) {
        if (power != null) return
        val pm = ctx.getSystemService(PowerManager::class.java) ?: return
        power = pm
        pm.addThermalStatusListener(ctx.mainExecutor, listener)
        thermal(pm.currentThermalStatus)
    }

    fun stopHeat() {
        power?.removeThermalStatusListener(listener)
        power = null
        if (hot) { hot = false; onChange?.invoke() }
    }

    // Severe or worse: hot. Back under moderate: cool again (a gap, so it does not flap).
    private fun thermal(status: Int) {
        val headroom = runCatching { power?.getThermalHeadroom(10) ?: Float.NaN }.getOrDefault(Float.NaN)
        val now = when {
            status >= PowerManager.THERMAL_STATUS_SEVERE || (!headroom.isNaN() && headroom > 0.85f) -> true
            status <= PowerManager.THERMAL_STATUS_LIGHT && (headroom.isNaN() || headroom < 0.7f) -> false
            else -> hot
        }
        Log.i(TAG, "thermal status $status, headroom $headroom${if (now != hot) ", now ${if (now) "hot" else "cool"}" else ""}")
        if (now != hot) {
            hot = now
            onChange?.invoke()
        }
    }

    /**
     * A performance hint session for the calling thread: say how long each pass may take, then report
     * how long it took, so the CPU speeds up before a frame is late. Null when the phone has none.
     */
    class Hint(ctx: Context, targetNs: Long) {
        private val session = runCatching {
            ctx.getSystemService(PerformanceHintManager::class.java)?.createHintSession(intArrayOf(Process.myTid()), targetNs)
        }.getOrNull()
        private var target = targetNs

        fun target(ns: Long) {
            if (ns == target) return
            target = ns
            runCatching { session?.updateTargetWorkDuration(ns) }
        }

        fun report(ns: Long) {
            if (ns > 0) runCatching { session?.reportActualWorkDuration(ns) }
        }

        fun powerEfficient(on: Boolean) {
            if (android.os.Build.VERSION.SDK_INT >= 35) runCatching { session?.setPreferPowerEfficiency(on) }
        }

        fun close() = runCatching { session?.close() }
    }
}
