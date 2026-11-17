package dev.hyperwin.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.os.Bundle
import android.text.InputType
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Guest display and controls. The console text is kept in real views for accessibility. */
class MainActivity : Activity() {
    private val console = StringBuilder()
    private var desktopSurface: SurfaceView? = null
    private var guestDisplay: GuestDisplay? = null
    private lateinit var status: TextView
    private lateinit var typeButton: Button
    private lateinit var bootButton: Button
    private lateinit var stopButton: Button
    private lateinit var logButton: Button
    private lateinit var menuButton: Button
    private lateinit var controls: LinearLayout
    private lateinit var logPanel: ScrollView
    private lateinit var consoleText: TextView
    private var machineDialog: MachinePrep.Dialog? = null
    private val prefs by lazy { getSharedPreferences("hyperwin", MODE_PRIVATE) }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val desktopHasFocus = desktopSurface?.hasFocus() == true
        if (desktopHasFocus && event.keyCode !in HOST_KEYS &&
            (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP)
        ) {
            if (AvfLaunch.key(event)) return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        status = TextView(this).apply {
            text = "Ready. Start Windows to open the guest."
            contentDescription = "Status: $text"
            setTextColor(0xFFFFFFFF.toInt())
            textSize = 16f
            accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            isScreenReaderFocusable = true
            isFocusable = true
        }
        consoleText = TextView(this).apply {
            text = "Guest console output will appear here."
            setTextColor(0xFFE7EEF2.toInt())
            textSize = 12f
            typeface = android.graphics.Typeface.MONOSPACE
            setTextIsSelectable(true)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            contentDescription = "Guest console output"
            setPadding(dp(12), dp(8), dp(12), dp(8))
        }

        val surface = SurfaceView(this).apply {
            contentDescription = "Windows guest display. Touch moves the Windows pointer and clicks. " +
                "Two fingers right-click. A hardware keyboard and mouse also work."
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            isFocusable = true
            isFocusableInTouchMode = true
            isClickable = true
            setOnClickListener { requestFocus() }
            requestUnbufferedDispatch(InputDevice.SOURCE_ANY)
            setOnTouchListener { view, event ->
                if (event.actionMasked == MotionEvent.ACTION_DOWN) view.requestFocus()
                if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) {
                    VsockInput.mouse(event, view.width, view.height)
                } else {
                    VsockInput.touch(event, view.width, view.height)
                }
                if (event.actionMasked == MotionEvent.ACTION_UP) view.performClick()
                true
            }
            setOnGenericMotionListener { view, event ->
                if (isMouseEvent(event)) VsockInput.mouse(event, view.width, view.height) else false
            }
        }
        desktopSurface = surface
        guestDisplay = GuestDisplay(surface) { message ->
            runOnUiThread { if (!isFinishing) updateStatus(message) }
        }

        bootButton = Button(this).apply {
            text = "Boot Windows"
            setOnClickListener { startWindows() }
        }
        typeButton = Button(this).apply {
            text = "Type to Windows"
            contentDescription = "Open the Android keyboard to type into Windows"
            setOnClickListener { showTypeTextDialog() }
        }
        stopButton = Button(this).apply {
            text = "Stop VM"
            contentDescription = "Stop the Windows virtual machine"
            isEnabled = false
            setOnClickListener {
                isEnabled = false
                Thread {
                    val report = AvfLaunch.stopWindows { guestDisplay?.vmStopped() }
                    runOnUiThread {
                        updateStatus(report)
                        stopButton.isEnabled = false
                        bootButton.isEnabled = true
                        typeButton.isEnabled = false
                        showControls(true)
                    }
                }.start()
            }
        }

        logButton = Button(this).apply {
            text = "Show log"
            setOnClickListener {
                val show = logPanel.visibility != View.VISIBLE
                logPanel.visibility = if (show) View.VISIBLE else View.GONE
                text = if (show) "Hide log" else "Show log"
            }
        }
        controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(10), dp(12), dp(8))
            setBackgroundColor(0xDD101418.toInt())
            addView(status)
            addView(CheckBox(this@MainActivity).apply {
                text = "Open to Windows on launch"
                contentDescription = "Start Windows as soon as HyperWin opens"
                setTextColor(0xFFFFFFFF.toInt())
                isChecked = prefs.getBoolean(BOOT_ON_LAUNCH, false)
                setOnCheckedChangeListener { _, on -> prefs.edit().putBoolean(BOOT_ON_LAUNCH, on).apply() }
            })
            addView(Button(this@MainActivity).apply {
                text = "Prepare machine"
                contentDescription = "Build a Windows disk from an ISO, with cores, memory and disk size"
                setOnClickListener { prepareMachine() }
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(bootButton, LinearLayout.LayoutParams(0, dp(52), 1f).apply { marginEnd = dp(6) })
                addView(typeButton, LinearLayout.LayoutParams(0, dp(52), 1f).apply { marginEnd = dp(6) })
                addView(logButton, LinearLayout.LayoutParams(0, dp(52), 1f).apply { marginEnd = dp(6) })
                addView(stopButton, LinearLayout.LayoutParams(0, dp(52), 1f))
            }, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(8) })
        }
        logPanel = ScrollView(this).apply {
            isFillViewport = true
            visibility = View.GONE
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            contentDescription = "Scrollable guest console output"
            setBackgroundColor(0xDD101418.toInt())
            addView(consoleText)
        }
        menuButton = Button(this).apply {
            text = "Menu"
            contentDescription = "Show HyperWin controls"
            alpha = 0.6f
            visibility = View.GONE
            setOnClickListener { showControls(controls.visibility != View.VISIBLE) }
        }
        val root = FrameLayout(this).apply {
            setBackgroundColor(0xff000000.toInt())
            addView(surface, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            addView(controls, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                android.view.Gravity.TOP,
            ))
            addView(logPanel, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                dp(160),
                android.view.Gravity.BOTTOM,
            ))
            addView(menuButton, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                dp(44),
                android.view.Gravity.TOP or android.view.Gravity.END,
            ))
            setOnApplyWindowInsetsListener { _, insets ->
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                controls.setPadding(controls.paddingLeft, dp(10) + bars.top,
                    controls.paddingRight, controls.paddingBottom)
                (menuButton.layoutParams as FrameLayout.LayoutParams).apply {
                    topMargin = bars.top + dp(4)
                    marginEnd = bars.right + dp(4)
                }
                val logParams = logPanel.layoutParams as FrameLayout.LayoutParams
                logParams.bottomMargin = bars.bottom
                logPanel.layoutParams = logParams
                insets
            }
        }
        setContentView(root)
        bootButton.requestFocus()
        if (intent.getBooleanExtra("windows", false) ||
            (savedInstanceState == null && AvfLaunch.machine == null && prefs.getBoolean(BOOT_ON_LAUNCH, false))
        ) startWindows()
    }

    @Deprecated("Activity result for the ISO picker")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: android.content.Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == MachinePrep.PICK_ISO && resultCode == RESULT_OK && uri != null) machineDialog?.onIsoPicked(uri)
    }

    private fun prepareMachine() {
        machineDialog = MachinePrep.Dialog(this) { machine ->
            bootButton.isEnabled = false
            updateStatus("Preparing ${machine.name}. This takes a while.")
            Thread {
                val report = runCatching {
                    MachinePrep.prepare(this, machine) { percent, message ->
                        runOnUiThread { if (!isFinishing) updateStatus("$percent%: $message") }
                    }
                }.getOrElse { it.message ?: "Machine preparation failed" }
                runOnUiThread {
                    updateStatus(report)
                    bootButton.isEnabled = AvfLaunch.machine == null
                }
            }.start()
        }.also { it.show() }
    }

    override fun onPause() {
        guestDisplay?.saveFrame()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        if (AvfLaunch.machine != null) guestDisplay?.vmStarted(AvfLaunch.sharedWindow)
    }

    private fun startWindows() {
        val surface = desktopSurface ?: return
        bootButton.isEnabled = false
        updateStatus("Starting Windows firmware. Keyboard input is available after the guest starts.")
        surface.requestFocus()
        Thread {
            val report = runCatching {
                AvfLaunch.bootWindows(
                    this,
                    onVmStarted = { guestDisplay?.vmStarted(AvfLaunch.sharedWindow) },
                    onVmStopped = {
                        guestDisplay?.vmStopped()
                        runOnUiThread {
                            if (!isFinishing) {
                                bootButton.isEnabled = true
                                typeButton.isEnabled = false
                                stopButton.isEnabled = false
                                updateStatus("Windows stopped.")
                                showControls(true)
                            }
                        }
                    },
                ) { chunk ->
                    synchronized(console) {
                        console.append(chunk)
                        if (console.length > 12000) console.delete(0, console.length - 12000)
                    }
                    val shown = currentConsole()
                    runOnUiThread {
                        consoleText.text = shown
                    }
                }
            }.getOrElse { it.message ?: "Windows boot failed" }
            runOnUiThread {
                updateStatus(report.substringBefore('\n'))
                if (report.contains('\n')) {
                    consoleText.text = report.substringAfter('\n')
                }
                val isRunning = report.startsWith("VM running;")
                bootButton.isEnabled = !isRunning
                typeButton.isEnabled = isRunning
                stopButton.isEnabled = isRunning
                showControls(!isRunning)
                surface.requestFocus()
            }
        }.start()
    }

    private fun showControls(show: Boolean) {
        controls.visibility = if (show) View.VISIBLE else View.GONE
        menuButton.visibility = if (AvfLaunch.machine != null) View.VISIBLE else View.GONE
        menuButton.text = if (show) "Hide" else "Menu"
        if (!show) {
            logPanel.visibility = View.GONE
            logButton.text = "Show log"
            desktopSurface?.requestFocus()
        }
    }

    private fun showTypeTextDialog() {
        if (AvfLaunch.machine == null) {
            updateStatus("Start Windows before typing to the guest.")
            return
        }
        val textInput = EditText(this).apply {
            hint = "Text to send to Windows"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            contentDescription = "Text to send to the Windows guest"
            minLines = 2
            maxLines = 5
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("Type to Windows")
            .setMessage("Basic US keyboard characters are submitted as key presses. The guest layout and Caps Lock can affect the result.")
            .setView(textInput)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Send", null)
            .create()
        dialog.setOnDismissListener {
            desktopSurface?.post { desktopSurface?.requestFocus() }
        }
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val text = textInput.text
                if (text.isEmpty()) {
                    dialog.dismiss()
                } else {
                    typeButton.isEnabled = false
                    dialog.dismiss()
                    Thread {
                        val result = AvfLaunch.typeText(text)
                        runOnUiThread {
                            if (!isFinishing) {
                                typeButton.isEnabled = true
                                val message = when {
                                    result.failure == AvfLaunch.TextSendFailure.GUEST_UNAVAILABLE ->
                                        "Windows is not accepting text input right now."
                                    result.failure == AvfLaunch.TextSendFailure.UNSUPPORTED_CHARACTER ->
                                        "No text was sent. Use basic US keyboard characters."
                                    result.failure == AvfLaunch.TextSendFailure.INPUT_REJECTED && result.acceptedCharacters > 0 ->
                                        "Only ${result.acceptedCharacters} of ${result.totalCharacters} characters were submitted before input stopped."
                                    result.failure == AvfLaunch.TextSendFailure.INPUT_REJECTED ->
                                        "The Windows guest rejected the text input."
                                    else -> "All ${result.totalCharacters} characters were submitted as guest key presses."
                                }
                                updateStatus(message)
                            }
                        }
                    }.start()
                }
            }
        }
        dialog.show()
        textInput.post {
            textInput.requestFocus()
            val inputMethod = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethod.showSoftInput(textInput, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    private fun updateStatus(message: String) {
        status.text = message
        status.contentDescription = "Status: $message"
    }

    private fun currentConsole(): String = synchronized(console) { console.toString().takeLast(12000) }

    private fun isMouseEvent(event: MotionEvent): Boolean =
        (event.source and InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private companion object {
        const val BOOT_ON_LAUNCH = "bootOnLaunch"
        val HOST_KEYS = setOf(
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_HOME,
            KeyEvent.KEYCODE_POWER,
            KeyEvent.KEYCODE_VOLUME_UP,
            KeyEvent.KEYCODE_VOLUME_DOWN,
            KeyEvent.KEYCODE_VOLUME_MUTE,
        )
    }
}
