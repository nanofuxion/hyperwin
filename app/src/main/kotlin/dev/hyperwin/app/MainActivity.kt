package dev.hyperwin.app

import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.DynamicColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.CircularProgressIndicator
import dev.hyperwin.app.Ui.dp
import dev.hyperwin.app.Ui.fill
import dev.hyperwin.app.Ui.spaced
import dev.hyperwin.app.Ui.themeColor
import com.google.android.material.R as M

/**
 * One activity, three screens: the first-run setup slides, the settings (home) screen, and Windows
 * itself with a small pull-down toolbar. Settings are kept in the machine's machine.json and in
 * the app's preferences, so they are there again on the next launch.
 */
class MainActivity : AppCompatActivity(), HomeScreen.Actions {
    private enum class Screen { SETUP, HOME, WINDOWS }

    private val console = StringBuilder()
    private val prefs by lazy { getSharedPreferences("hyperwin", MODE_PRIVATE) }
    private val ui = Handler(Looper.getMainLooper())
    private var screen = Screen.HOME
    private var status = "Ready to start."
    private var replaceOldMachines = false
    private var keyboardHeight = 0
    private var starting = false

    private lateinit var root: FrameLayout
    private lateinit var guest: GuestView
    private lateinit var guestDisplay: GuestDisplay
    private lateinit var guestLayer: FrameLayout
    private lateinit var booting: LinearLayout
    private lateinit var bootingText: TextView
    private lateinit var toolbar: FrameLayout
    private lateinit var handle: View
    private lateinit var bar: View
    private lateinit var homeLayer: FrameLayout
    private lateinit var setupLayer: FrameLayout
    private lateinit var home: HomeScreen
    private var setup: SetupFlow? = null

    private val pickIso = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) setup?.onIsoPicked(uri)
    }
    private val pickDxvk = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@registerForActivityResult
        Thread {
            val (id, error) = runCatching { HwGpu.importDxvk(this, uri) }.getOrElse { null to (it.message ?: "Could not read that file.") }
            runOnUiThread {
                if (id == null) {
                    MaterialAlertDialogBuilder(this).setTitle("Not a DXVK for this app")
                        .setMessage(error).setPositiveButton("OK", null).show()
                    return@runOnUiThread
                }
                HwGpu.useDxvk(this, id)
                HwGpu.restart()
                home.refreshGraphics()
            }
        }.start()
    }
    private val pickDriver = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@registerForActivityResult
        Thread {
            val name = runCatching { HwGpu.importDriver(this, uri) }.getOrNull()
            runOnUiThread {
                if (name == null) {
                    MaterialAlertDialogBuilder(this).setTitle("No driver found")
                        .setMessage("Choose a Vulkan driver .so file, or a .zip that holds one.")
                        .setPositiveButton("OK", null).show()
                    return@runOnUiThread
                }
                HwGpu.useDriver(this, name)
                HwGpu.restart()
                if (screen == Screen.HOME) home.refresh(AvfLaunch.machine != null, status)
            }
        }.start()
    }
    private val requestMic = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        GuestAudio.micChanged(this)
    }
    private val requestNotifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        if (it) WindowsService.refresh(this)
    }
    private var micAsked = false
    private var notificationsAsked = false
    private val controls by lazy { InputControlsView(this).apply { visibility = View.GONE } }

    private fun toggleControls(edit: Boolean) {
        if (controls.visibility == View.VISIBLE && controls.editMode == edit) {
            controls.editMode = false; controls.visibility = View.GONE; return
        }
        TouchControlsManager.ensureDefaults(this)
        val all = TouchControlsManager.listProfiles(this)
        controls.profile = TouchControlsManager.getProfile(this, TouchControlsManager.activeProfileId(this)) ?: all.firstOrNull()
        controls.overlayOpacity = TouchControlsManager.overlayOpacity(this)
        controls.blockOutsideTouch = TouchControlsManager.blockOutsideTouch(this)
        controls.onProfileChanged = { controls.profile?.let { TouchControlsManager.saveProfile(this, it) } }
        controls.editMode = edit
        controls.visibility = View.VISIBLE
    }

    private val importIcp = registerForActivityResult(androidx.activity.result.contract.ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) try {
            val text = contentResolver.openInputStream(uri)!!.use { it.readBytes().decodeToString() }
            val p = TouchControlsManager.importProfile(this, text)
            TouchControlsManager.setActiveProfileId(this, p.id)
            controls.profile = p
            Toast.makeText(this, "Imported ${p.name}", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) { Toast.makeText(this, "Not a Winlator controls file", Toast.LENGTH_SHORT).show() }
    }

    private fun menu(anchor: View, vararg actions: Pair<String, () -> Unit>) {
        ui.removeCallbacks(collapseToolbar)
        android.widget.PopupMenu(this, anchor).apply {
            actions.forEachIndexed { i, a -> menu.add(0, i, i, a.first) }
            setOnMenuItemClickListener { actions[it.itemId].second(); true }
            setOnDismissListener { nudgeToolbar() }
        }.show()
    }

    private fun controlsMenu(anchor: View) {
        val shown = controls.visibility == View.VISIBLE && !controls.editMode
        menu(anchor,
            (if (shown) "Hide touch controls" else "Show touch controls") to { toggleControls(false) },
            "Edit touch controls" to { toggleControls(true) },
            "Profiles…" to { pickProfile() })
    }

    private fun pickProfile() {
        TouchControlsManager.ensureDefaults(this)
        val all = TouchControlsManager.listProfiles(this)
        val apply = { p: ControlsProfile ->
            TouchControlsManager.setActiveProfileId(this, p.id)
            controls.profile = p
        }
        MaterialAlertDialogBuilder(this).setTitle("Controls profile")
            .setItems((all.map { it.name } + listOf("New copy of current", "Rename current", "Copy current to clipboard", "Import from clipboard", "Import Winlator .icp file")).toTypedArray()) { _, i ->
                val cur = controls.profile ?: all.firstOrNull()
                val cm = getSystemService(android.content.ClipboardManager::class.java)
                if (i < all.size) apply(all[i])
                else when (i - all.size) {
                    0 -> cur?.let { apply(TouchControlsManager.duplicateProfile(this, it)) }
                    1 -> cur?.let { p ->
                        val input = android.widget.EditText(this).apply { setText(p.name) }
                        MaterialAlertDialogBuilder(this).setTitle("Rename").setView(input)
                            .setPositiveButton("Save") { _, _ -> TouchControlsManager.renameProfile(this, p, input.text.toString()) }
                            .setNegativeButton("Cancel", null).show()
                    }
                    2 -> cur?.let {
                        cm.setPrimaryClip(android.content.ClipData.newPlainText("controls", TouchControlsManager.exportProfileJson(it)))
                        Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show()
                    }
                    4 -> importIcp.launch("*/*")
                    else -> try {
                        apply(TouchControlsManager.importProfile(this, cm.primaryClip?.getItemAt(0)?.text.toString()))
                    } catch (e: Exception) { Toast.makeText(this, "Clipboard has no controls profile", Toast.LENGTH_SHORT).show() }
                }
            }
            .setNegativeButton("Delete current") { _, _ ->
                controls.profile?.let { TouchControlsManager.deleteProfile(this, it.id) }
                controls.profile = TouchControlsManager.listProfiles(this).firstOrNull()
            }
            .setPositiveButton("Close", null).show()
    }

    private val collapseToolbar = Runnable { showBar(false) }
    private val hideBooting = Runnable { booting.visibility = View.GONE }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (screen == Screen.WINDOWS && GamepadInput.onKey(event)) return true
        if (screen == Screen.WINDOWS && event.keyCode !in HOST_KEYS &&
            (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP) && AvfLaunch.key(event)
        ) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        VsockInput.touchMode = if (prefs.getBoolean(TRACKPAD, false)) VsockInput.TouchMode.TRACKPAD else VsockInput.TouchMode.TOUCH
        VsockInput.gameSpeed = prefs.getFloat(GAME_SPEED, 1f)
        GamepadInput.register(this)
        // A mouse plugged in or taken out: Windows takes it over, or lets it go.
        getSystemService(android.hardware.input.InputManager::class.java).registerInputDeviceListener(
            object : android.hardware.input.InputManager.InputDeviceListener {
                override fun onInputDeviceAdded(id: Int) = updateCapture()
                override fun onInputDeviceRemoved(id: Int) = updateCapture()
                override fun onInputDeviceChanged(id: Int) = updateCapture()
            }, ui)
        // A fresh process holds no VM, so one still running from an earlier session cannot be resumed.
        if (AvfLaunch.machine == null && VmAccess.granted(this)) Thread { AvfLaunch.killStale(this) }.start()

        guest = GuestView(this).apply {
            contentDescription = "Windows display. Touch works as a touch screen, or as a trackpad in trackpad mode. " +
                "A hardware keyboard and mouse also work."
            isFocusable = true
            isFocusableInTouchMode = true
            requestUnbufferedDispatch(InputDevice.SOURCE_ANY)
            setOnTouchListener { view, event ->
                if (event.actionMasked == MotionEvent.ACTION_DOWN) view.requestFocus()
                if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) VsockInput.mouse(event, view.width, view.height)
                else VsockInput.touch(event, view.width, view.height)
                if (event.actionMasked == MotionEvent.ACTION_UP) {
                    view.performClick()
                    if (keyboardHeight > 0) panForKeyboard()
                }
                true
            }
            // Windows draws the pointer; the phone's own would sit beside it.
            pointerIcon = android.view.PointerIcon.getSystemIcon(this@MainActivity, android.view.PointerIcon.TYPE_NULL)
            setOnCapturedPointerListener { _, event -> VsockInput.captured(event) }
            setOnGenericMotionListener { view, event ->
                if (GamepadInput.isGamepad(event.source)) GamepadInput.onMotion(event)
                else if ((event.source and InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE) VsockInput.mouse(event, view.width, view.height)
                else false
            }
        }
        guestLayer = FrameLayout(this).apply {
            setBackgroundColor(0xff000000.toInt())
            addView(guest, FrameLayout.LayoutParams(Ui.MATCH, Ui.MATCH, Gravity.CENTER))
            addView(controls, FrameLayout.LayoutParams(Ui.MATCH, Ui.MATCH))
        }
        guestDisplay = GuestDisplay(guest) {
            // The first changed frame after the firmware's blank one is Windows drawing.
            runOnUiThread { ui.removeCallbacks(hideBooting); hideBooting.run() }
            GameSettings.loading(this, false)
        }
        bootingText = Ui.text(this, M.attr.textAppearanceTitleMedium).apply { setTextColor(0xffffffff.toInt()) }
        booting = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            visibility = View.GONE
            addView(CircularProgressIndicator(this@MainActivity).apply { isIndeterminate = true },
                LinearLayout.LayoutParams(Ui.WRAP, Ui.WRAP))
            addView(bootingText.spaced(20, Ui.WRAP))
        }
        buildToolbar()
        // The layers carry the surface colour, so it also fills behind the system bars and cutout.
        homeLayer = FrameLayout(this).apply { setBackgroundColor(themeColor(M.attr.colorSurface)) }
        setupLayer = FrameLayout(this).apply { setBackgroundColor(themeColor(M.attr.colorSurface)) }
        home = HomeScreen(this, prefs, this)
        homeLayer.fill(home.view)

        root = FrameLayout(this).apply {
            setBackgroundColor(0xff000000.toInt())
            fill(guestLayer)
            fill(booting)
            addView(toolbar, FrameLayout.LayoutParams(Ui.WRAP, Ui.WRAP, Gravity.TOP or Gravity.CENTER_HORIZONTAL))
            fill(homeLayer)
            fill(setupLayer)
        }
        setContentView(root)
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            val keyboard = insets.getInsets(WindowInsetsCompat.Type.ime()).bottom
            listOf(homeLayer, setupLayer).forEach { it.setPadding(bars.left, bars.top, bars.right, maxOf(bars.bottom, keyboard)) }
            keyboardHeight = keyboard
            panForKeyboard()
            (toolbar.layoutParams as FrameLayout.LayoutParams).topMargin = bars.top
            toolbar.requestLayout()
            insets
        }
        // Run the phone's screen at its fastest rate, so Windows frames are shown as soon as they change,
        // or at 60 Hz in the Game Dashboard's Battery mode or while the phone is hot.
        applyDisplayMode()
        GameSettings.onChange = {
            applyDisplayMode()
            if (GameSettings.hot && screen == Screen.WINDOWS)
                android.widget.Toast.makeText(this, "Phone is hot, limiting to 60 fps", android.widget.Toast.LENGTH_LONG).show()
        }
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                when {
                    screen == Screen.SETUP && setup?.back() == true -> Unit
                    screen == Screen.SETUP && preparedMachine() != null -> {
                        replaceOldMachines = false
                        show(Screen.HOME)
                    }
                    screen == Screen.HOME && AvfLaunch.machine != null -> show(Screen.WINDOWS)
                    screen == Screen.WINDOWS -> showBar(true)       // never drop out of a running Windows
                    else -> finish()
                }
            }
        })

        val isoExtra = intent.getStringExtra(EXTRA_PREPARE_ISO)
        when {
            AvfLaunch.machine != null -> {                          // the activity was rebuilt under a running VM
                show(Screen.WINDOWS)
                guestDisplay.vmStarted(AvfLaunch.sharedWindow)
            }
            savedInstanceState == null && isoExtra != null -> {
                // Unattended setup (adb): am start -n dev.hyperwin.app/.MainActivity --es prepareIso <uri>
                val machine = Machine.create(this).apply {
                    source = isoExtra
                    sourceName = isoExtra.substringAfterLast('/')
                    save()
                }
                openSetup().showWorking()
                runPrep(machine)
            }
            preparedMachine() == null -> openSetup()                // first launch: the setup slides
            intent.getBooleanExtra("windows", false) || prefs.getBoolean(BOOT_ON_LAUNCH, false) -> startWindows()
            else -> show(Screen.HOME)
        }
    }

    override fun onResume() {
        super.onResume()
        FileShare.publish(this)   // the all-files permission may have just been granted
        GuestAudio.micChanged(this)   // so may the microphone
        GuestAudio.askMic = { runOnUiThread { askMic() } }
        if (GuestAudio.micWanted) askMic()
        GuestAgent.onGuestRebootNeeded = { runOnUiThread { askGuestReboot() } }
        if (GuestAgent.guestRebootNeeded) askGuestReboot()
        GamepadInput.resume()
        if (AvfLaunch.machine != null) guestDisplay.vmStarted(AvfLaunch.sharedWindow)
        if (screen == Screen.WINDOWS) immersive(true)
        applyDisplayMode()   // the Game Dashboard mode may have changed meanwhile
    }

    private fun applyDisplayMode() {
        GameSettings.displayMode(this, display)?.let { mode ->
            if (window.attributes.preferredDisplayModeId != mode.modeId)
                window.attributes = window.attributes.also { it.preferredDisplayModeId = mode.modeId }
        }
    }

    override fun onPause() {
        super.onPause()
        GuestAudio.askMic = null
        GuestAgent.onGuestRebootNeeded = null
        GamepadInput.pause()
    }

    // ---- a plugged-in mouse belongs to Windows ----
    // While the Windows screen is up, a mouse is held by the app (pointer capture): Android's pointer is hidden
    // and cannot click the app around Windows, and Windows gets relative counts, as from a PC's mouse, so it
    // works across several Windows screens and in games. Touch the screen for the app's own controls.

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        updateCapture()
    }

    private fun mousePresent() = android.view.InputDevice.getDeviceIds().any { id ->
        val d = android.view.InputDevice.getDevice(id) ?: return@any false
        !d.isVirtual && d.supportsSource(android.view.InputDevice.SOURCE_MOUSE) &&
            !d.supportsSource(android.view.InputDevice.SOURCE_TOUCHPAD)
    }

    private fun updateCapture() {
        if (!::guest.isInitialized) return
        val want = screen == Screen.WINDOWS && hasWindowFocus() && mousePresent()
        if (want && !guest.hasPointerCapture()) {
            guest.requestFocus()
            guest.requestPointerCapture()
        } else if (!want && guest.hasPointerCapture()) {
            guest.releasePointerCapture()
        }
    }

    /** Once per launch, when Windows has opened the microphone. */
    private fun askMic() {
        if (micAsked || GuestAudio.micPermitted(this)) return
        micAsked = true
        requestMic.launch(android.Manifest.permission.RECORD_AUDIO)
    }

    /** Once per launch on Android 13+, so WindowsService's ongoing notification stays visible. */
    private fun askNotifications() {
        if (notificationsAsked || android.os.Build.VERSION.SDK_INT < 33) return
        if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) == android.content.pm.PackageManager.PERMISSION_GRANTED) return
        notificationsAsked = true
        requestNotifications.launch(android.Manifest.permission.POST_NOTIFICATIONS)
    }

    private fun preparedMachine(): Machine? =
        Machine.active(this)?.takeIf { it.prepared }
            ?: Machine.all(this).firstOrNull { it.prepared }?.also { Machine.setActive(this, it) }

    private fun show(next: Screen) {
        screen = next
        setupLayer.visibility = if (next == Screen.SETUP) View.VISIBLE else View.GONE
        homeLayer.visibility = if (next == Screen.HOME) View.VISIBLE else View.GONE
        toolbar.visibility = if (next == Screen.WINDOWS) View.VISIBLE else View.GONE
        if (next == Screen.HOME) {
            home.refresh(AvfLaunch.machine != null, status)
            // At most once a day, and never over the Windows screen.
            Updater.check(this, manual = false) { screen == Screen.HOME }
        }
        if (next == Screen.WINDOWS) {
            showBar(false)
            guest.requestFocus()
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            askNotifications()
        } else {
            guest.hideKeyboard()
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        immersive(next == Screen.WINDOWS)
        updateCapture()
        if (next == Screen.WINDOWS) GameSettings.watchHeat(this) else GameSettings.stopHeat()
        if (next != Screen.WINDOWS) GamepadInput.stopAllRumble()
    }

    private fun immersive(on: Boolean) {
        val controller = WindowCompat.getInsetsController(window, root)
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        if (on) controller.hide(WindowInsetsCompat.Type.systemBars()) else controller.show(WindowInsetsCompat.Type.systemBars())
    }

    // ---- toolbar over Windows: a small grabber that opens into a pill of actions ----

    private fun buildToolbar() {
        handle = FrameLayout(this).apply {
            contentDescription = "Show HyperWin controls"
            isClickable = true
            isFocusable = true
            addView(View(this@MainActivity).apply {
                background = GradientDrawable().apply {
                    cornerRadius = dp(8).toFloat()
                    setColor(0xb0ffffff.toInt())
                    setStroke(dp(1), 0x80000000.toInt())
                }
            }, FrameLayout.LayoutParams(dp(44), dp(7), Gravity.CENTER))
            setOnClickListener { showBar(true) }
        }
        fun action(label: String, icon: Int, onClick: () -> Unit) =
            Ui.button(this, label, M.attr.borderlessButtonStyle, icon) { onClick(); nudgeToolbar() }
        bar = MaterialCardView(this, null, M.attr.materialCardViewElevatedStyle).apply {
            radius = dp(28).toFloat()
            // See-through, so Windows stays readable underneath. No shadow: it would show through.
            cardElevation = 0f
            setCardBackgroundColor(androidx.core.graphics.ColorUtils.setAlphaComponent(themeColor(M.attr.colorSurfaceContainerHigh), 0xB0))
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(8), dp(2), dp(4), dp(2))
                addView(action("Keyboard", R.drawable.ic_keyboard) {}.apply {
                    contentDescription = "Choose this phone's keyboard or the one inside Windows"
                    setOnClickListener { menu(it, "Android keyboard" to { togglePhoneKeyboard() }, "Windows keyboard" to { showWindowsKeyboard() }) }
                })
                addView(action("Mouse", R.drawable.ic_drag) {}.apply {
                    contentDescription = "Choose how fingers on the screen act: touch screen or trackpad"
                    setOnClickListener {
                        val pad = VsockInput.touchMode == VsockInput.TouchMode.TRACKPAD
                        fun mode(trackpad: Boolean) {
                            VsockInput.touchMode = if (trackpad) VsockInput.TouchMode.TRACKPAD else VsockInput.TouchMode.TOUCH
                            prefs.edit().putBoolean(TRACKPAD, trackpad).apply()
                        }
                        menu(it,
                            (if (!pad) "✓ " else "") + "Touch screen (multi-touch)" to { mode(false) },
                            (if (pad) "✓ " else "") + "Trackpad (moves the mouse, in games too)" to { mode(true) })
                    }
                })
                addView(action("Shortcuts", R.drawable.ic_shortcuts) {}.apply {
                    contentDescription = "Send a Windows key shortcut"
                    setOnClickListener {
                        menu(it,
                            "Task Manager (Ctrl + Shift + Esc)" to { AvfLaunch.shortcut(KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_ESCAPE) },
                            "Reset graphics driver (Win + Ctrl + Shift + B)" to { AvfLaunch.shortcut(KeyEvent.KEYCODE_META_LEFT, KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_B) },
                            "Switch window (Alt + Tab)" to { AvfLaunch.shortcut(KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_TAB) },
                            "Close window (Alt + F4)" to { AvfLaunch.shortcut(KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_F4) })
                    }
                })
                addView(action("Controls", R.drawable.ic_gamepad) {}.also { b -> b.setOnClickListener { controlsMenu(b) } })
                addView(action("Settings", R.drawable.ic_settings) { show(Screen.HOME) })
                addView(Ui.iconButton(this@MainActivity, R.drawable.ic_close, "Hide controls") { showBar(false) })
            })
        }
        toolbar = FrameLayout(this).apply {
            addView(handle, FrameLayout.LayoutParams(dp(88), dp(30), Gravity.TOP or Gravity.CENTER_HORIZONTAL))
            addView(bar, FrameLayout.LayoutParams(Ui.WRAP, Ui.WRAP, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
                setMargins(dp(8), dp(8), dp(8), dp(12))
            })
        }
    }

    private fun showBar(open: Boolean) {
        ui.removeCallbacks(collapseToolbar)
        bar.visibility = if (open) View.VISIBLE else View.GONE
        handle.visibility = if (open) View.GONE else View.VISIBLE
        if (open) nudgeToolbar()
    }

    /** The pill folds back into the grabber after a few quiet seconds. */
    private fun nudgeToolbar() {
        ui.removeCallbacks(collapseToolbar)
        ui.postDelayed(collapseToolbar, 6000)
    }

    /**
     * The phone's keyboard covers the lower part of the screen. Windows keeps its size and slides
     * up just far enough that the place last touched (or the pointer) sits in the part still visible.
     */
    private fun panForKeyboard() {
        val visible = root.height - keyboardHeight
        val focus = guest.top + VsockInput.focusY * guest.height
        val most = (guest.bottom - visible).coerceAtLeast(0).toFloat()
        val shift = if (keyboardHeight == 0) 0f else (focus - visible / 2f).coerceIn(0f, most)
        guest.animate().translationY(-shift).setDuration(150).start()
    }

    private fun togglePhoneKeyboard() {
        val shown = ViewCompat.getRootWindowInsets(root)?.isVisible(WindowInsetsCompat.Type.ime()) == true
        if (shown) guest.hideKeyboard() else guest.showKeyboard()
    }

    private fun showWindowsKeyboard() {
        // Windows' touch keyboard, docked along the bottom. Older agents can only open the classic one.
        if (GuestAgent.connected && GuestAgent.agentVersion >= 4) GuestAgent.exec("C:\\HyperWin\\hwagent.exe keyboard", hidden = true)
        else if (GuestAgent.connected) GuestAgent.exec("explorer.exe C:\\Windows\\System32\\osk.exe", desktop = true)
        else Toast.makeText(this, "Windows is still starting. Try again in a moment.", Toast.LENGTH_SHORT).show()
    }

    // ---- setup ----

    private fun openSetup(): SetupFlow {
        val flow = SetupFlow(this, pickIso = { pickIso.launch(arrayOf("application/x-iso9660-image", "application/octet-stream", "*/*")) }) {
            runPrep(it)
        }
        setup = flow
        setupLayer.removeAllViews()
        setupLayer.fill(flow.view)
        show(Screen.SETUP)
        return flow
    }

    /** Builds the machine's disk, then boots it. */
    private fun runPrep(machine: Machine) {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        Thread {
            val report = runCatching {
                MachinePrep.prepare(this, machine) { percent, message ->
                    Log.i("HyperWinPrep", "$percent% $message")
                    runOnUiThread { setup?.showProgress(percent, message) }
                }
            }.getOrElse { it.message ?: "Windows could not be set up." }
            Log.i("HyperWinPrep", report)
            runOnUiThread {
                if (isFinishing) return@runOnUiThread
                if (!machine.prepared) {
                    window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    setup?.showFailure(report)
                    return@runOnUiThread
                }
                // A reinstall the user confirmed: the Windows it replaces goes now that the new one exists.
                if (replaceOldMachines) Machine.all(this).filter { it.dir != machine.dir }.forEach { it.dir.deleteRecursively() }
                replaceOldMachines = false
                setup = null
                setupLayer.removeAllViews()
                if (AvfLaunch.machine == null) startWindows() else show(Screen.HOME)
            }
        }.start()
    }

    // ---- Windows ----

    private fun startWindows(restart: Boolean = false, attempt: Int = 1) {
        if (starting || AvfLaunch.machine != null) return     // one Windows at a time: they share a disk
        GameSettings.loading(this, true)
        if (!VmAccess.granted(this)) { VmAccess.ensure(this) { startWindows(restart, attempt) }; return }
        if (!restart && attempt == 1 && !skipGuestPatch) {
            val m = preparedMachine()
            if (m != null && MachinePrep.guestNeedsPatch(this, m)) {
                askOfflinePatch(m)
                return
            }
        }
        starting = true
        show(Screen.WINDOWS)
        bootingText.text = if (restart) "Windows is restarting" else "Starting Windows"
        booting.visibility = View.VISIBLE
        ui.removeCallbacks(hideBooting)
        ui.postDelayed(hideBooting, 60_000)
        Thread {
            var retryable = true
            val report = runCatching {
                AvfLaunch.bootWindows(
                    this,
                    onVmStarted = { DisplayWatchdog.vmStarted(); guestDisplay.vmStarted(AvfLaunch.sharedWindow) },
                    onVmStopped = {
                        guestDisplay.vmStopped()
                        runOnUiThread {
                            // A stop while still starting is reported, and handled, by the start below.
                            if (isFinishing || starting) return@runOnUiThread
                            if (AvfLaunch.lastStopWasReboot) {
                                startWindows(restart = true)
                            } else {
                                status = "Windows is off."
                                booting.visibility = View.GONE
                                show(Screen.HOME)
                            }
                        }
                    },
                ) { chunk ->
                    synchronized(console) {
                        console.append(chunk)
                        if (console.length > 12000) console.delete(0, console.length - 12000)
                    }
                }
            }.getOrElse { t ->
                val root = generateSequence(t) { it.cause }.last()
                if (root is ReflectiveOperationException || root is SecurityException || root is UnsupportedOperationException) {
                    retryable = false
                }
                t.message ?: "Windows could not start."
            }
            runOnUiThread {
                if (isFinishing) return@runOnUiThread
                starting = false
                if (report == AvfLaunch.BUSY) {
                    // Another screen of this app is starting it already; show that one's Windows.
                    status = "Windows is running."
                } else if (report.startsWith("VM running;")) {
                    status = "Windows is running."
                } else if (report.startsWith("stopped") && AvfLaunch.lastStopWasReboot) {
                    startWindows(restart = true)
                } else if (retryable && attempt < 4) {
                    // The firmware sometimes faults at power-on (ArmCpuDxe, splitting a live page
                    // table entry under KVM), and sometimes twice running. A try costs about a second.
                    synchronized(console) { console.append("\n").append(report).append("\nTrying once more.\n") }
                    startWindows(restart, attempt + 1)
                } else {
                    synchronized(console) { console.append("\n").append(report) }
                    status = "Windows could not start. The log under Advanced has the details."
                    booting.visibility = View.GONE
                    show(Screen.HOME)
                }
            }
        }.start()
    }

    override fun startOrReturn() {
        if (AvfLaunch.machine != null) show(Screen.WINDOWS) else startWindows()
    }

    /** Asks Windows to shut down by itself; the screen comes back here when it has. */
    override fun shutDown() {
        if (!GuestAgent.connected) {
            MaterialAlertDialogBuilder(this)
                .setTitle("Windows is not answering yet")
                .setMessage("It may still be starting. You can wait, or force it off. Forcing loses anything not saved.")
                .setNegativeButton("Wait", null)
                .setPositiveButton("Force stop") { _, _ -> forceStop() }
                .show()
            return
        }
        GuestAgent.exec("shutdown /s /t 0")
        status = "Windows is shutting down."
        home.setStatus(status)
    }

    override fun forceStop() {
        Thread {
            AvfLaunch.stopWindows { guestDisplay.vmStopped() }
            runOnUiThread {
                status = "Windows is off."
                booting.visibility = View.GONE
                show(Screen.HOME)
            }
        }.start()
    }

    override fun showLogs() {
        val text = synchronized(console) { console.toString() }.ifBlank { "Nothing yet. The log fills when Windows starts." }
        MaterialAlertDialogBuilder(this)
            .setTitle("Logs")
            .setView(ScrollView(this).apply {
                addView(TextView(this@MainActivity).apply {
                    this.text = text
                    typeface = android.graphics.Typeface.MONOSPACE
                    textSize = 12f
                    setTextIsSelectable(true)
                    setPadding(dp(24), dp(8), dp(24), dp(8))
                })
            })
            .setNeutralButton("Copy") { _, _ ->
                getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("HyperWin log", text))
            }
            .setPositiveButton("Close", null)
            .show()
    }

    override fun importDxvk() = pickDxvk.launch(arrayOf("application/zip", "application/gzip", "application/x-gzip", "application/x-tar", "application/octet-stream", "*/*"))

    override fun importDriver() = pickDriver.launch(arrayOf("application/octet-stream", "application/zip", "application/x-sharedlib", "*/*"))

    private var guestPatchDialog: AlertDialog? = null
    private var skipGuestPatch = false
    private var offlinePatchRunning = false

    private fun askOfflinePatch(machine: Machine) {
        if (isFinishing || isDestroyed || guestPatchDialog?.isShowing == true || offlinePatchRunning) return
        if (screen != Screen.HOME) show(Screen.HOME)
        guestPatchDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Update Windows?")
            .setMessage("This version of HyperWin includes updated drivers and tools for Windows. Patch Windows before starting?")
            .setNegativeButton("Start without updating") { _, _ ->
                skipGuestPatch = true
                startWindows()
            }
            .setPositiveButton("Update and start") { _, _ -> runOfflinePatch(machine, thenStart = true) }
            .show()
    }

    private fun askGuestReboot() {
        if (isFinishing || isDestroyed || guestPatchDialog?.isShowing == true) return
        GuestAgent.clearRebootNeeded()
        guestPatchDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Windows drivers updated")
            .setMessage("Restart Windows for the new drivers to take effect.")
            .setNegativeButton("Later", null)
            .setPositiveButton("Restart now") { _, _ -> GuestAgent.exec("shutdown /r /t 0") }
            .show()
    }

    override fun updateGuestDrivers() {
        val m = preparedMachine() ?: return
        if (AvfLaunch.machine != null) {
            MaterialAlertDialogBuilder(this)
                .setTitle("Shut down Windows first")
                .setMessage("Driver updates are applied before Windows starts.")
                .setPositiveButton("OK", null)
                .show()
            return
        }
        if (MachinePrep.guestNeedsPatch(this, m)) {
            runOfflinePatch(m, thenStart = false)
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Reapply Windows drivers?")
            .setMessage("Windows already matches this version of HyperWin. Reapply the HyperWin drivers and tools anyway?")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Reapply") { _, _ -> runOfflinePatch(m, thenStart = false) }
            .show()
    }

    private fun runOfflinePatch(machine: Machine, thenStart: Boolean) {
        if (offlinePatchRunning) return
        offlinePatchRunning = true
        val progress = MaterialAlertDialogBuilder(this)
            .setTitle("Updating Windows")
            .setMessage("Preparing update files…")
            .setCancelable(false)
            .show()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        Thread {
            val report = runCatching {
                MachinePrep.patchGuest(this, machine) { percent, message ->
                    runOnUiThread {
                        if (!isFinishing && !isDestroyed) {
                            progress.setMessage(if (percent > 0) "$message $percent%" else message)
                        }
                    }
                }
            }.getOrElse { it.message ?: "Something went wrong." }
            runOnUiThread {
                offlinePatchRunning = false
                runCatching { progress.dismiss() }
                if (AvfLaunch.machine == null) window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                if (isFinishing || isDestroyed) return@runOnUiThread
                if (screen == Screen.HOME) home.refresh(AvfLaunch.machine != null, status)
                if (report == "ok") {
                    skipGuestPatch = false
                    if (thenStart) startWindows()
                    else Toast.makeText(this, "Windows drivers are up to date.", Toast.LENGTH_LONG).show()
                } else {
                    val b = MaterialAlertDialogBuilder(this)
                        .setTitle("Windows update did not finish")
                        .setMessage(report)
                    if (thenStart) {
                        b.setNegativeButton("Cancel", null)
                            .setPositiveButton("Start anyway") { _, _ ->
                                skipGuestPatch = true
                                startWindows()
                            }
                    } else {
                        b.setPositiveButton("OK", null)
                    }
                    b.show()
                }
            }
        }.start()
    }

    override fun reinstall() {
        replaceOldMachines = true
        openSetup()
    }

    companion object {
        const val BOOT_ON_LAUNCH = "bootOnLaunch"
        const val TRACKPAD = "trackpadMode"
        const val GAME_SPEED = "gameMouseSpeed"
        private const val EXTRA_PREPARE_ISO = "prepareIso"
        private val HOST_KEYS = setOf(
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_HOME,
            KeyEvent.KEYCODE_POWER,
            KeyEvent.KEYCODE_VOLUME_UP,
            KeyEvent.KEYCODE_VOLUME_DOWN,
            KeyEvent.KEYCODE_VOLUME_MUTE,
        )
    }
}
