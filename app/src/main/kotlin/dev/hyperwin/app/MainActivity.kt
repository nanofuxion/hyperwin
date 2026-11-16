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
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Guest display and controls. The console text is kept in real views for accessibility. */
class MainActivity : Activity() {
    private val console = StringBuilder()
    private var desktopSurface: DesktopSurfaceView? = null
    private var guestDisplay: GuestDisplay? = null
    private lateinit var status: TextView
    private lateinit var mouseStatus: TextView
    private lateinit var mouseButton: Button
    private lateinit var typeButton: Button
    private lateinit var bootButton: Button
    private lateinit var stopButton: Button
    private lateinit var consoleText: TextView
    private var mouseCaptured = false
    private var escapeCaptureKeyUpPending = false

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val surface = desktopSurface
        if (event.keyCode == KeyEvent.KEYCODE_ESCAPE && (mouseCaptured || escapeCaptureKeyUpPending)) {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> {
                    escapeCaptureKeyUpPending = true
                    surface?.releasePointerCapture()
                    return true
                }
                KeyEvent.ACTION_UP -> if (escapeCaptureKeyUpPending) {
                    escapeCaptureKeyUpPending = false
                    return true
                }
            }
        }
        val desktopHasFocus = surface?.hasFocus() == true || mouseCaptured
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
        mouseStatus = TextView(this).apply {
            text = "Mouse is not captured."
            contentDescription = "Mouse status: $text"
            setTextColor(0xFFE7EEF2.toInt())
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

        val surface = DesktopSurfaceView(this) { captured -> setMouseCaptured(captured) }.apply {
            contentDescription = "Windows guest display. Tap to focus, then use a hardware keyboard. " +
                "Use Capture mouse before moving a mouse into the guest."
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            isFocusable = true
            isFocusableInTouchMode = true
            isClickable = true
            setOnClickListener { requestFocus() }
            requestUnbufferedDispatch(InputDevice.SOURCE_ANY)
            setOnTouchListener { view, event ->
                if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) {
                    false // Mouse movement is relative only after pointer capture.
                } else {
                    if (event.actionMasked == MotionEvent.ACTION_DOWN) view.requestFocus()
                    AvfLaunch.touch(event)
                    if (event.actionMasked == MotionEvent.ACTION_UP) view.performClick()
                    true
                }
            }
            setOnCapturedPointerListener { _, event ->
                // AVF interprets captured ACTION_MOVE coordinates as relative mouse deltas.
                AvfLaunch.mouse(event)
            }
            setOnGenericMotionListener { _, event ->
                if (isMouseEvent(event) && event.actionMasked == MotionEvent.ACTION_SCROLL) {
                    AvfLaunch.mouse(event)
                } else {
                    false
                }
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
        mouseButton = Button(this).apply {
            text = "Capture mouse"
            setOnClickListener {
                if (AvfLaunch.machine == null) {
                    updateStatus("Start Windows before capturing the mouse.")
                    return@setOnClickListener
                }
                val target = desktopSurface ?: return@setOnClickListener
                if (mouseCaptured) {
                    target.releasePointerCapture()
                } else {
                    target.requestFocus()
                    target.requestPointerCapture()
                }
            }
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
                        mouseButton.isEnabled = false
                        typeButton.isEnabled = false
                        setMouseCaptured(false)
                    }
                }.start()
            }
        }

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(10), dp(12), dp(8))
            setBackgroundColor(0xDD101418.toInt())
            addView(status)
            addView(mouseStatus, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(4) })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(bootButton, LinearLayout.LayoutParams(0, dp(52), 1f).apply {
                    marginEnd = dp(6)
                })
                addView(mouseButton, LinearLayout.LayoutParams(0, dp(52), 1f).apply {
                    marginEnd = dp(6)
                })
                addView(typeButton, LinearLayout.LayoutParams(0, dp(52), 1f))
                addView(stopButton, LinearLayout.LayoutParams(0, dp(52), 1f).apply {
                    marginStart = dp(6)
                })
            }, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(8) })
        }
        val logPanel = ScrollView(this).apply {
            isFillViewport = true
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            contentDescription = "Scrollable guest console output"
            setBackgroundColor(0xDD101418.toInt())
            addView(consoleText)
        }
        val root = FrameLayout(this).apply {
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
                dp(96),
                android.view.Gravity.BOTTOM,
            ))
            setOnApplyWindowInsetsListener { _, insets ->
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                controls.setPadding(controls.paddingLeft, dp(10) + bars.top,
                    controls.paddingRight, controls.paddingBottom)
                val logParams = logPanel.layoutParams as FrameLayout.LayoutParams
                logParams.bottomMargin = bars.bottom
                logPanel.layoutParams = logParams
                insets
            }
        }
        setContentView(root)
        bootButton.requestFocus()
        if (intent.getBooleanExtra("windows", false)) startWindows()
    }

    override fun onPause() {
        desktopSurface?.releasePointerCapture()
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
                    onVmStopped = { guestDisplay?.vmStopped() },
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
                mouseButton.isEnabled = isRunning
                typeButton.isEnabled = isRunning
                stopButton.isEnabled = isRunning
                surface.requestFocus()
            }
        }.start()
    }

    private fun setMouseCaptured(captured: Boolean) {
        mouseCaptured = captured
        if (!::mouseButton.isInitialized || !::mouseStatus.isInitialized) return
        mouseButton.text = if (captured) "Release mouse" else "Capture mouse"
        val message = if (captured) {
            "Mouse captured. Press Escape or select Release mouse to return it to Android."
        } else {
            "Mouse is not captured."
        }
        mouseStatus.text = message
        mouseStatus.contentDescription = "Mouse status: $message"
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

    private class DesktopSurfaceView(
        context: Context,
        private val onCaptureChanged: (Boolean) -> Unit,
    ) : SurfaceView(context) {
        override fun onPointerCaptureChange(hasCapture: Boolean) {
            super.onPointerCaptureChange(hasCapture)
            onCaptureChanged(hasCapture)
        }
    }

    private companion object {
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
