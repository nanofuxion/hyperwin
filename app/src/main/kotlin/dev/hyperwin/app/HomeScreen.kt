package dev.hyperwin.app

import android.app.ActivityManager
import android.content.Context
import android.content.SharedPreferences
import android.content.res.ColorStateList
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.ImageView
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
        fun importDriver()
        fun importDxvk()
        fun updateGuestDrivers()
    }

    private val title = Ui.text(ctx, M.attr.textAppearanceDisplaySmall)
    private val status = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorOnSurfaceVariant).apply {
        accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
    }
    private val specs = Ui.text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorOnSurfaceVariant)
    private val primary: MaterialButton = Ui.button(ctx, "Start Windows", icon = R.drawable.ic_play) { actions.startOrReturn() }
    private val shutDown: MaterialButton = Ui.button(ctx, "Shut down", M.attr.materialButtonOutlinedStyle, R.drawable.ic_power) { actions.shutDown() }
    private val restartNote = Ui.text(ctx, M.attr.textAppearanceBodySmall, M.attr.colorOnSurfaceVariant,
        "Cores and memory apply the next time Windows starts. Screen size changes at once with the HyperWin screen driver, otherwise at the next start.")
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
        listOf(display(machine), graphics(), input(), network(machine), files(), performance(machine), startup(), advanced(running, machine)).forEach {
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
                    // With the HyperWin screen driver, a running Windows switches at once.
                    if (AvfLaunch.machine != null && Machine.active(ctx)?.dir == machine.dir) AvfLaunch.sharedWindow?.let { w ->
                        val (sw, sh) = machine.frameSize()
                        val native = AvfPlan.nativeScreen()
                        w.withBuffer { WindowBridge.nativeSetScreen(it, sw, sh, native.first, native.second, true) }
                    }
                    row.setSummary(sizeLabel(machine))
                    showSpecs(machine)
                    dialog.dismiss()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        val vsync = Ui.switchRow(ctx, "Limit to screen refresh",
            "Fullscreen games draw one frame per refresh of the phone's screen. Smoother and cooler. Off: games choose, and may draw frames nobody sees.",
            HwGpu.vsync(ctx)) { on -> HwGpu.setVsync(ctx, on) }
        val cool = Ui.switchRow(ctx, "Keep the phone cool",
            "When the phone gets hot, the screen drops to 60 Hz until it cools down. Off: it only says so.",
            prefs.getBoolean(GameSettings.KEEP_COOL, true)) { on ->
            prefs.edit().putBoolean(GameSettings.KEEP_COOL, on).apply()
            GameSettings.onChange?.invoke()
        }
        return Ui.section(ctx, "Display", row, vsync, cool)
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

    /**
     * The Vulkan stack the GPU renders with, top first: fixes, which stack and can be reordered, over
     * the one driver that is on. Changes apply at once, even while Windows runs. Drivers added from
     * files have a remove button; the built-in entries do not.
     */
    private fun graphics(): View {
        val list = HwGpu.stack(ctx).toMutableList()
        fun signature() = list.filter { it.on }.map { it.id }
        var applied = signature()
        fun apply() {
            HwGpu.saveStack(ctx, list)
            if (signature() == applied) return
            applied = signature()
            HwGpu.restart()
            if (HwGpu.running) setStatus("GPU restarted with ${HwGpu.describe(ctx)}. Programs using it need to start again.")
        }
        val box = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        lateinit var render: () -> Unit

        fun row(e: HwGpu.Entry): LinearLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = ctx.dp(64)
            setPadding(ctx.dp(8), ctx.dp(6), ctx.dp(8), ctx.dp(6))
            setBackgroundColor(themeColor(M.attr.colorSurfaceContainer))
            val row = this
            addView(ImageView(ctx).apply {
                setImageResource(R.drawable.ic_drag)
                imageTintList = ColorStateList.valueOf(themeColor(M.attr.colorOnSurfaceVariant))
                contentDescription = "Drag ${e.label} up or down"
                setPadding(ctx.dp(12), ctx.dp(12), ctx.dp(12), ctx.dp(12))
                setOnTouchListener(dragHandle(row, box, list) { apply() })
            }, LinearLayout.LayoutParams(ctx.dp(48), ctx.dp(48)))
            val summary = when (e.id) {
                "vortek" -> "Winlator's driver fixes. DXVK and vkd3d-proton need them on Mali and some other GPUs. Includes BC textures."
                "bcn" -> "Decodes compressed BC textures the driver lacks."
                HwGpu.PHONE -> "This phone's own driver."
                else -> "A driver added from a file."
            }
            addView(Ui.column(ctx,
                Ui.text(ctx, M.attr.textAppearanceBodyLarge, value = e.label),
                Ui.text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorOnSurfaceVariant, (if (e.fix) "Fix. " else "Driver. ") + summary),
            ), LinearLayout.LayoutParams(0, Ui.WRAP, 1f).apply { marginStart = ctx.dp(4); marginEnd = ctx.dp(8) })
            addView(Ui.switch(ctx, e.on) { on ->
                val at = list.indexOfFirst { it.id == e.id }
                when {
                    e.fix -> list[at] = list[at].copy(on = on)
                    on -> list.replaceAll { if (it.fix) it else it.copy(on = it.id == e.id) }
                    // A driver must stay on: the phone's takes over, and stays on by itself.
                    else -> list.replaceAll { if (it.fix) it else it.copy(on = it.id == HwGpu.PHONE) }
                }
                post { render() }
                apply()
            }.apply { contentDescription = "${e.label} on" })
            if (e.builtIn) addView(View(ctx), LinearLayout.LayoutParams(ctx.dp(48), ctx.dp(48)))
            else addView(Ui.iconButton(ctx, R.drawable.ic_close, "Remove ${e.label}") {
                MaterialAlertDialogBuilder(ctx)
                    .setTitle("Remove ${e.label}?")
                    .setMessage("You can add it again from its file later.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Remove") { _, _ ->
                        HwGpu.deleteDriver(ctx, e.id)
                        list.clear()
                        list += HwGpu.stack(ctx)
                        render()
                        apply()
                    }
                    .show()
            }, LinearLayout.LayoutParams(ctx.dp(48), ctx.dp(48)))
        }
        render = {
            box.removeAllViews()
            list.forEach { box.addView(row(it), LinearLayout.LayoutParams(Ui.MATCH, Ui.WRAP)) }
        }
        render()
        return Ui.section(ctx, "Graphics",
            box,
            Ui.row(ctx, "Add a driver file", "A Vulkan driver as a .so, or a .zip that holds one, such as Turnip or PanVK.", Ui.chevron(ctx)) {
                actions.importDriver()
            },
            direct3d(),
            Ui.row(ctx, "Add a DXVK file", "A Direct3D layer built for HyperWin: a .zip with meta.json and libdxvk_*.so (gpu/host/hwd3d/build.sh makes one).", Ui.chevron(ctx)) {
                actions.importDxvk()
            },
            shaderCacheRow(),
            Ui.text(ctx, M.attr.textAppearanceBodySmall, M.attr.colorOnSurfaceVariant,
                "Fixes that are on run top first, over the one driver that is on. Drag to reorder. " +
                    "Changes apply at once. Programs using the GPU need to start again.").apply {
                setPadding(ctx.dp(20), ctx.dp(4), ctx.dp(20), ctx.dp(12))
            })
    }

    private var direct3dBox: LinearLayout? = null

    /** Which DXVK draws Direct3D: exactly one on. Switching restarts the renderer, like a driver swap. */
    private fun direct3d(): View {
        val box = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        direct3dBox = box
        fillDirect3d()
        return box
    }

    fun refreshGraphics() = fillDirect3d()

    private fun fillDirect3d() {
        val box = direct3dBox ?: return
        box.removeAllViews()
        // Desktop always stays on the built-in DXVK-Sarek (shown, not changeable).
        box.addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = ctx.dp(64)
            setPadding(ctx.dp(20), ctx.dp(6), ctx.dp(20), ctx.dp(6))
            setBackgroundColor(themeColor(M.attr.colorSurfaceContainer))
            addView(Ui.column(ctx,
                Ui.text(ctx, M.attr.textAppearanceBodyLarge, value = "Desktop: ${HwGpu.DXVK_SHIPPED_NAME}, built in"),
                Ui.text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorOnSurfaceVariant,
                    "Phone's own driver, no add-ons"),
            ), LinearLayout.LayoutParams(0, Ui.WRAP, 1f))
        }, LinearLayout.LayoutParams(Ui.MATCH, Ui.WRAP))
        val choice = HwGpu.dxvkChoice(ctx)
        for (d in HwGpu.dxvkList(ctx)) {
            box.addView(LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                minimumHeight = ctx.dp(64)
                setPadding(ctx.dp(20), ctx.dp(6), ctx.dp(8), ctx.dp(6))
                setBackgroundColor(themeColor(M.attr.colorSurfaceContainer))
                addView(Ui.column(ctx,
                    Ui.text(ctx, M.attr.textAppearanceBodyLarge, value = "Games: ${d.name}"),
                    Ui.text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorOnSurfaceVariant,
                        if (d.builtIn) "Default for games (built in)." else "Added from a file. Only affects games."),
                ), LinearLayout.LayoutParams(0, Ui.WRAP, 1f).apply { marginEnd = ctx.dp(8) })
                addView(Ui.switch(ctx, d.id == choice) { on ->
                    if (on && d.id != choice) {
                        HwGpu.useDxvk(ctx, d.id)
                        HwGpu.restart()
                        if (HwGpu.running) setStatus("Games now use ${d.name}. Desktop stays on ${HwGpu.DXVK_SHIPPED_NAME}.")
                    }
                    post { fillDirect3d() }
                }.apply { contentDescription = "Games ${d.name} on" })
                if (d.builtIn) addView(View(ctx), LinearLayout.LayoutParams(ctx.dp(48), ctx.dp(48)))
                else addView(Ui.iconButton(ctx, R.drawable.ic_close, "Remove ${d.name}") {
                    MaterialAlertDialogBuilder(ctx)
                        .setTitle("Remove ${d.name}?")
                        .setMessage("You can add it again from its file later.")
                        .setNegativeButton("Cancel", null)
                        .setPositiveButton("Remove") { _, _ ->
                            val was = d.id == choice
                            HwGpu.deleteDxvk(ctx, d.id)
                            if (was) HwGpu.restart()
                            fillDirect3d()
                        }
                        .show()
                }, LinearLayout.LayoutParams(ctx.dp(48), ctx.dp(48)))
            }, LinearLayout.LayoutParams(Ui.MATCH, Ui.WRAP))
        }
    }

    /** Compiled shaders kept between runs, with a Clear button for when Windows is off. */
    private fun shaderCacheRow(): View {
        fun summary() = HwGpu.shaderCacheSummary(ctx) ?: "Empty. Shaders games compile are kept here, so they load fast next time."
        lateinit var row: LinearLayout
        val clear = Ui.button(ctx, "Clear", M.attr.borderlessButtonStyle) {
            if (AvfLaunch.machine != null) {
                MaterialAlertDialogBuilder(ctx).setTitle("Windows is running")
                    .setMessage("Shut Windows down first. The graphics driver has the cache open while it runs.")
                    .setPositiveButton("OK", null).show()
                return@button
            }
            HwGpu.clearShaderCache(ctx)
            row.setSummary(summary())
        }
        row = Ui.row(ctx, "Shader cache", summary(), clear)
        return row
    }

    /**
     * Drag by the handle: the row follows the finger and trades places with a neighbour once past
     * its middle. Fixes only trade with fixes and drivers with drivers, since fixes always run on top.
     */
    private fun dragHandle(row: View, box: LinearLayout, list: MutableList<HwGpu.Entry>, done: () -> Unit): View.OnTouchListener {
        var lastY = 0f
        return View.OnTouchListener { handle, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    handle.parent.requestDisallowInterceptTouchEvent(true)
                    row.elevation = ctx.dp(6).toFloat()
                    lastY = ev.rawY
                }
                MotionEvent.ACTION_MOVE -> {
                    row.translationY += ev.rawY - lastY
                    lastY = ev.rawY
                    val at = box.indexOfChild(row)
                    val next = box.getChildAt(at + 1)
                    val prev = box.getChildAt(at - 1)
                    fun same(i: Int) = i in list.indices && list[i].fix == list[at].fix
                    if (next != null && same(at + 1) && row.translationY > next.height / 2f) {
                        java.util.Collections.swap(list, at, at + 1)
                        box.removeView(next)
                        box.addView(next, at)
                        row.translationY -= next.height
                    } else if (prev != null && same(at - 1) && row.translationY < -prev.height / 2f) {
                        java.util.Collections.swap(list, at, at - 1)
                        box.removeView(prev)
                        box.addView(prev, at)
                        row.translationY += prev.height
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    row.animate().translationY(0f).withEndAction { row.elevation = 0f }.start()
                    done()
                }
            }
            true
        }
    }

    private fun input() = Ui.section(ctx, "Touch and mouse",
        Ui.switchRow(ctx, "Trackpad mode",
            "The screen works as a laptop touchpad that moves the mouse, in games too: tap to click, tap and drag, two fingers to scroll. " +
                "Off: it is a touch screen.",
            prefs.getBoolean(MainActivity.TRACKPAD, false)) { on ->
            prefs.edit().putBoolean(MainActivity.TRACKPAD, on).apply()
            VsockInput.touchMode = if (on) VsockInput.TouchMode.TRACKPAD else VsockInput.TouchMode.TOUCH
        },
        Ui.switchRow(ctx, "Rumble", "Games' rumble reaches the controllers, and the phone as set below. Off: no rumble at all.",
            prefs.getBoolean(GamepadInput.RUMBLE, true)) { on ->
            prefs.edit().putBoolean(GamepadInput.RUMBLE, on).apply()
            if (!on) GamepadInput.stopAllRumble()
        },
        Ui.row(ctx, "Player 1 rumble on the phone",
            "Auto: when player 1's controller has no motors, like most clip-on ones. Uses the media vibration strength."),
        Ui.segmented(ctx, listOf("Auto", "Phone", "Controller only"),
            GamepadInput.PHONE_RUMBLE_MODES.indexOf(prefs.getString(GamepadInput.PHONE_RUMBLE, "auto")).coerceAtLeast(0)) { i ->
            prefs.edit().putString(GamepadInput.PHONE_RUMBLE, GamepadInput.PHONE_RUMBLE_MODES[i]).apply()
        },
        Ui.row(ctx, "Player 1 controller",
            "Xbox works with the most games. DualShock 4 adds gyro aiming: tilt the phone to aim in Steam, " +
                "SDL games and emulators that read a DualShock 4's motion. Other players are always Xbox."),
        Ui.segmented(ctx, listOf("Xbox", "DualShock 4"),
            if (prefs.getString(GamepadInput.PLAYER1, "xbox") == "ds4") 1 else 0) { i ->
            prefs.edit().putString(GamepadInput.PLAYER1, if (i == 1) "ds4" else "xbox").apply()
            GamepadInput.player1Changed()
        },
        Ui.row(ctx, "Mouse speed", "The trackpad and a plugged-in mouse. 1x is a mouse's own counts."),
        Ui.slider(ctx, 0.25f, 4f, 0.25f, prefs.getFloat(MainActivity.GAME_SPEED, 1f), { "%.2fx".format(it) }) { v ->
            prefs.edit().putFloat(MainActivity.GAME_SPEED, v).apply()
            VsockInput.gameSpeed = v
        })

    private fun network(machine: Machine) = Ui.section(ctx, "Network",
        Ui.switchRow(ctx, "Use this phone's Wi-Fi address",
            "Other devices on your network can reach Windows directly. Turn on for LAN games.",
            machine.lanMode) { on ->
            machine.lanMode = on
            machine.save()
            GuestAgent.setLanMode(on)
        })

    private fun files() = Ui.section(ctx, "Files",
        Ui.switchRow(ctx, "Phone storage in Windows",
            if (FileShare.permitted()) "Internal storage, SD cards and USB drives show up as drives in Windows."
            else "Shows internal storage, SD cards and USB drives as drives in Windows. Needs access to all files.",
            FileShare.enabled(ctx) && FileShare.permitted()) { on ->
            prefs.edit().putBoolean(FileShare.PREF, on).apply()
            if (on && !FileShare.permitted()) FileShare.askPermission(ctx)
            FileShare.publish(ctx)
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
        val cpuStatus = Ui.text(ctx, M.attr.textAppearanceBodySmall, M.attr.colorOnSurfaceVariant,
            if (ExperimentalCpuPerformance.enabled(ctx) && !GuestAgent.connected) "Will apply when Windows connects" else ExperimentalCpuPerformance.status)
        val listener: (String) -> Unit = { cpuStatus.text = if (ExperimentalCpuPerformance.enabled(ctx) && !GuestAgent.connected) "Will apply when Windows connects" else it }
        cpuStatus.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) { ExperimentalCpuPerformance.listen(listener) }
            override fun onViewDetachedFromWindow(v: View) { ExperimentalCpuPerformance.unlisten(listener) }
        })
        val experimental = Ui.switchRow(ctx, "Experimental CPU topology",
            "An experimental attempt to pass this phone's CPU topology into Windows, so it can see the little, mid, and big cores. Can increase heat and battery use. Applies now, or when Windows next connects.",
            ExperimentalCpuPerformance.enabled(ctx)) { on -> ExperimentalCpuPerformance.setEnabled(ctx, on) }
        experimental.gravity = Gravity.TOP
        (experimental.getChildAt(1).layoutParams as LinearLayout.LayoutParams).topMargin = ctx.dp(2)
        cpuStatus.setPadding(ctx.dp(20), 0, ctx.dp(20), ctx.dp(12))
        return Ui.section(ctx, "Performance", cores, memory, experimental, cpuStatus)
    }

    private fun startup() = Ui.section(ctx, "When HyperWin opens",
        Ui.row(ctx, "Open to", "", Ui.segmented(ctx, listOf("Settings", "Windows"),
            if (prefs.getBoolean(MainActivity.BOOT_ON_LAUNCH, false)) 1 else 0) {
            prefs.edit().putBoolean(MainActivity.BOOT_ON_LAUNCH, it == 1).apply()
        }))

    private fun advanced(running: Boolean, machine: Machine): View {
        val rows = mutableListOf<View>(
            Ui.row(ctx, "Logs", "What the virtual machine printed while starting.", Ui.chevron(ctx)) { actions.showLogs() },
        )
        if (running) {
            rows += Ui.row(ctx, "Force stop", "Turns Windows off at once, like pulling the plug.", Ui.chevron(ctx)) {
                MaterialAlertDialogBuilder(ctx)
                    .setTitle("Force stop Windows?")
                    .setMessage("Anything not saved in Windows is lost. Use Shut down when Windows still responds.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Force stop") { _, _ -> actions.forceStop() }
                    .show()
            }
        } else {
            rows += Ui.row(
                ctx,
                "Update Windows drivers",
                if (MachinePrep.guestNeedsPatch(ctx, machine)) "An update from this version of HyperWin is ready."
                else "Reapply HyperWin drivers and tools to Windows before starting.",
                Ui.chevron(ctx),
            ) { actions.updateGuestDrivers() }
            rows += Ui.row(ctx, "Reinstall Windows", "Builds a fresh Windows from an ISO. The current one is erased when the new one is ready.", Ui.chevron(ctx)) {
                MaterialAlertDialogBuilder(ctx)
                    .setTitle("Reinstall Windows?")
                    .setMessage("You will choose an ISO and settings again. Your current Windows, with its files and programs, is erased once the new one is ready.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Continue") { _, _ -> actions.reinstall() }
                    .show()
            }
        }
        if (Updater.enabled(ctx)) rows += Ui.row(ctx, "Check for updates", "HyperWin ${Updater.current(ctx)}", Ui.chevron(ctx)) {
            (ctx as? android.app.Activity)?.let { Updater.check(it, manual = true) }
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
