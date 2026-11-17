package dev.hyperwin.app

import android.content.Context
import android.text.InputType
import android.view.KeyEvent
import android.view.SurfaceView
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

/**
 * The Windows display. It is also a text target for the phone's on-screen keyboard: whatever the
 * keyboard produces is replayed to Windows as key presses, so typing goes straight through.
 */
class GuestView(context: Context) : SurfaceView(context) {
    private val keyboard get() = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager

    fun showKeyboard() {
        requestFocus()
        keyboard.showSoftInput(this, 0)
    }

    fun hideKeyboard() = keyboard.hideSoftInputFromWindow(windowToken, 0)

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(out: EditorInfo): InputConnection {
        // Plain text with no suggestions, so keys arrive one at a time and the keyboard stays short.
        // Never the landscape "extract" editor, which would cover Windows with a text box.
        out.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        out.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE
        return object : BaseInputConnection(this, false) {
            private var composing = ""

            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                composing = ""
                AvfLaunch.typeText(text, skipUnknown = true)
                return true
            }

            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                // Keyboards that compose anyway: send what was added, erase what was taken back.
                val now = text.toString()
                val same = composing.commonPrefixWith(now).length
                repeat(composing.length - same) { tap(KeyEvent.KEYCODE_DEL) }
                AvfLaunch.typeText(now.substring(same), skipUnknown = true)
                composing = now
                return true
            }

            override fun finishComposingText(): Boolean {
                composing = ""
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                repeat(beforeLength) { tap(KeyEvent.KEYCODE_DEL) }
                repeat(afterLength) { tap(KeyEvent.KEYCODE_FORWARD_DEL) }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                // Some keyboards send "@" as the 2 key with shift held and no shift press of its own.
                val char = event.unicodeChar
                if (char < 0x20) AvfLaunch.key(event)
                else if (event.action == KeyEvent.ACTION_DOWN) AvfLaunch.typeText(char.toChar().toString(), skipUnknown = true)
                return true
            }

            override fun performEditorAction(actionCode: Int): Boolean {
                tap(KeyEvent.KEYCODE_ENTER)
                return true
            }
        }
    }

    private fun tap(keyCode: Int) {
        AvfLaunch.key(KeyEvent(KeyEvent.ACTION_DOWN, keyCode))
        AvfLaunch.key(KeyEvent(KeyEvent.ACTION_UP, keyCode))
    }
}
