package dev.hyperwin.app

import android.app.ActivityManager
import android.content.Context
import android.content.SharedPreferences
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import dev.hyperwin.app.Ui.dp
import dev.hyperwin.app.Ui.setSummary
import dev.hyperwin.app.Ui.spaced
import dev.hyperwin.app.Ui.themeColor
import com.google.android.material.R as M

/** The settings screen: start Windows on the left, what can be changed on the right. */
class HomeScreen(
    private val ctx: Context,
    private val prefs: SharedPreferences,
    private val actions: Actions,
) {
    interface Actions {
        fun startOrReturn()
        fun shutDown()
        fun forceStop()
        fun showLogs()
        fun reinstall()
    }

    private val title = Ui.text(ctx, M.attr.textAppearanceDisplaySmall)
    private val status = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorOnSurfaceVariant).apply {
        accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
    }
    private val specs = Ui.text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorOnSurfaceVariant)
    private val primary: MaterialButton = Ui.button(ctx, "Start Windows", icon = R.drawable.ic_play) { actions.startOrReturn() }
    private val shutDown: MaterialButton = Ui.button(ctx, "Shut down", M.attr.materialButtonOutlinedStyle, R.drawable.ic_power) { actions.shutDown() }
    private val restartNote = Ui.text(ctx, M.attr.textAppearanceBodySmall, M.attr.colorOnSurfaceVariant,
        "Screen size, cores and memory apply the next time Windows starts.")
    private val settings = Ui.column(ctx)

    val view: View = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        setBackgroundColor(themeColor(M.attr.colorSurface))
        val left = Ui.column(ctx,
            Ui.text(ctx, M.attr.textAppearanceLabelLarge, M.attr.colorPrimary, "HYPERWIN").apply { letterSpacing = 0.12f },
            title.spaced(8),
            status.spaced(4),
            specs.spaced(12),
            primary.spaced(28),
            shutDown.spaced(8),
        ).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(ctx.dp(32), ctx.dp(16), ctx.dp(24), ctx.dp(16))
        }
        addView(left, LinearLayout.LayoutParams(0, Ui.MATCH, 0.38f))
        addView(ScrollView(ctx).apply {
            isFillViewport = true
            clipToPadding = false
            addView(settings.apply { setPadding(ctx.dp(8), ctx.dp(8), ctx.dp(24), ctx.dp(32)) })
        }, LinearLayout.LayoutParams(0, Ui.MATCH, 0.62f))
    }

    /** Rebuilds the settings for the active machine and the VM's state. */
    fun refresh(running: Boolean, statusText: String) {
        val machine = Machine.active(ctx)
        title.text = machine?.name ?: "Windows"
        status.text = statusText
        primary.text = if (running) "Return to Windows" else "Start Windows"
        primary.isEnabled = machine?.prepared == true
        shutDown.visibility = if (running) View.VISIBLE else View.GONE
        settings.removeAllViews()
        if (machine == null) return
        showSpecs(machine)
        listOf(display(machine), input(), network(machine), performance(machine), startup(), advanced(running)).forEach {
            settings.addView(it, LinearLayout.LayoutParams(Ui.MATCH, Ui.WRAP))
        }
        restartNote.visibility = if (running) View.VISIBLE else View.GONE
        settings.addView(restartNote.apply { setPadding(ctx.dp(20), ctx.dp(16), ctx.dp(20), 0) })
    }

    fun setStatus(text: String) { status.text = text }

    private fun showSpecs(machine: Machine) {
        val cores = if (machine.cpuCount > 1) "${Runtime.getRuntime().availableProcessors()} cores" else "1 core"
        specs.text = "${machine.screenSize.replace("x", " × ")}  ·  $cores  ·  ${gb(machine.memoryMb)} memory"
    }

    private fun gb(mb: Int) = if (mb % 1024 == 0) "${mb / 1024} GB" else "%.1f GB".format(mb / 1024f)

    private fun display(machine: Machine): View {
        lateinit var row: LinearLayout
        row = Ui.row(ctx, "Screen size", sizeLabel(machine), Ui.chevron(ctx)) {
            val sizes = screenSizes()
            MaterialAlertDialogBuilder(ctx)
                .setTitle("Windows screen size")
                .setSingleChoiceItems(sizes.map { "${it.second}  ·  ${it.first.replace("x", " × ")}" }.toTypedArray(),
                    sizes.indexOfFirst { it.first == machine.screenSize }) { dialog, which ->
                    machine.screenSize = sizes[which].first
                    machine.save()
                    row.setSummary(sizeLabel(machine))
                    showSpecs(machine)
                    dialog.dismiss()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        return Ui.section(ctx, "Display", row)
    }

    private fun sizeLabel(machine: Machine) =
        (screenSizes().firstOrNull { it.first == machine.screenSize }?.second?.plus("  ·  ") ?: "") +
            machine.screenSize.replace("x", " × ")

    /** Sizes that suit this phone: its own screen, the same shape smaller, and the classic 1280x720. */
    private fun screenSizes(): List<Pair<String, String>> {
        val bounds = (ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager).maximumWindowMetrics.bounds
        val w = maxOf(bounds.width(), bounds.height())
        val h = minOf(bounds.width(), bounds.height())
        fun shaped(height: Int) = "${(height * w / h) / 2 * 2}x$height"
        return listOf("${w}x$h" to "Match this phone", shaped(900) to "Phone shape, medium",
            shaped(720) to "Phone shape, small", "1280x720" to "Classic")
            .filter { (size, _) -> size.split('x').let { AvfPlan.validFrame(it[0].toInt(), it[1].toInt()) } }
            .distinctBy { it.first }
    }

    private fun input() = Ui.section(ctx, "Touch",
        Ui.switchRow(ctx, "Trackpad mode",
            "Your finger moves the pointer. Tap to click, two fingers to scroll or right-click. Off: the screen is a touch screen.",
            prefs.getBoolean(MainActivity.TRACKPAD, false)) { on ->
            prefs.edit().putBoolean(MainActivity.TRACKPAD, on).apply()
            VsockInput.touchMode = if (on) VsockInput.TouchMode.TRACKPAD else VsockInput.TouchMode.TOUCH
        })

    private fun network(machine: Machine) = Ui.section(ctx, "Network",
        Ui.switchRow(ctx, "Use this phone's Wi-Fi address",
            "Other devices on your network can reach Windows directly. Turn on for LAN games.",
            machine.lanMode) { on ->
            machine.lanMode = on
            machine.save()
            GuestAgent.setLanMode(on)
        })

    private fun performance(machine: Machine): View {
        val hostCores = Runtime.getRuntime().availableProcessors()
        val cores = Ui.row(ctx, "Processor cores", "", Ui.segmented(ctx, listOf("1", "All $hostCores"), if (machine.cpuCount > 1) 1 else 0) {
            machine.cpuCount = if (it == 1) hostCores else 1
            machine.save()
            showSpecs(machine)
        })
        val value = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorPrimary, gb(machine.memoryMb))
        val slider = Ui.slider(ctx, Machine.MIN_MEMORY_MB.toFloat(), maxMemoryMb(ctx).toFloat(), 512f, machine.memoryMb.toFloat(), { gb(it.toInt()) }) {
            machine.memoryMb = it.toInt()
            machine.save()
            value.text = gb(machine.memoryMb)
            showSpecs(machine)
        }.apply { contentDescription = "Memory for Windows" }
        val memory = Ui.column(ctx,
            Ui.row(ctx, "Memory", "", value).apply { minimumHeight = ctx.dp(40) },
            slider.apply { setPadding(ctx.dp(12), 0, ctx.dp(12), ctx.dp(8)) },
        )
        return Ui.section(ctx, "Performance", cores, memory)
    }

    private fun startup() = Ui.section(ctx, "When HyperWin opens",
        Ui.row(ctx, "Open to", "", Ui.segmented(ctx, listOf("Settings", "Windows"),
            if (prefs.getBoolean(MainActivity.BOOT_ON_LAUNCH, false)) 1 else 0) {
            prefs.edit().putBoolean(MainActivity.BOOT_ON_LAUNCH, it == 1).apply()
        }))

    private fun advanced(running: Boolean): View {
        val rows = mutableListOf<View>(
            Ui.row(ctx, "Logs", "What the virtual machine printed while starting.", Ui.chevron(ctx)) { actions.showLogs() },
        )
        if (running) rows += Ui.row(ctx, "Force stop", "Turns Windows off at once, like pulling the plug.", Ui.chevron(ctx)) {
            MaterialAlertDialogBuilder(ctx)
                .setTitle("Force stop Windows?")
                .setMessage("Anything not saved in Windows is lost. Use Shut down when Windows still responds.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Force stop") { _, _ -> actions.forceStop() }
                .show()
        }
        else rows += Ui.row(ctx, "Reinstall Windows", "Builds a fresh Windows from an ISO. The current one is erased when the new one is ready.", Ui.chevron(ctx)) {
            MaterialAlertDialogBuilder(ctx)
                .setTitle("Reinstall Windows?")
                .setMessage("You will choose an ISO and settings again. Your current Windows, with its files and programs, is erased once the new one is ready.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Continue") { _, _ -> actions.reinstall() }
                .show()
        }
        return Ui.section(ctx, "Advanced", *rows.toTypedArray())
    }

    companion object {
        /** Most memory a machine may ask for: two thirds of the phone's, in 512 MB steps. */
        fun maxMemoryMb(ctx: Context): Int {
            val info = ActivityManager.MemoryInfo()
            (ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(info)
            val twoThirds = (info.totalMem / (1024 * 1024) * 2 / 3).toInt() / 512 * 512
            return maxOf(twoThirds, Machine.MIN_MEMORY_MB)
        }
    }
}
