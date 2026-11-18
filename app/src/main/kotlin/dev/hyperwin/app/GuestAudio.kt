package dev.hyperwin.app

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import java.nio.ByteBuffer

/**
 * Windows' speakers and microphone (audio/README.md). The sound itself never comes through here:
 * native code (cpp/audio_host.c) plays and records it with AAudio, straight out of and into the
 * shared window. This side starts and stops that, and handles the microphone permission, which
 * Android asks for the first time Windows opens the microphone. Without it Windows gets silence.
 */
object GuestAudio {
    init { System.loadLibrary("hyperwin") }

    /** Windows opened the microphone while the permission was missing. */
    @Volatile var micWanted = false
        private set

    /** Set by the screen in front, which can show the permission prompt. */
    @Volatile var askMic: (() -> Unit)? = null

    fun micPermitted(ctx: Context): Boolean =
        ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    /** With the window of a VM that is starting. */
    fun start(ctx: Context, window: ByteBuffer) = nativeStart(window, micPermitted(ctx))

    /** Before the window goes away. */
    fun stop() = nativeStop()

    /** After the permission may have changed. */
    fun micChanged(ctx: Context) {
        nativeSetMic(micPermitted(ctx))
        WindowsService.refresh(ctx)
    }

    /** From the native watcher thread. */
    @JvmStatic
    fun onMicWanted() {
        micWanted = true
        askMic?.invoke()
    }

    @JvmStatic private external fun nativeStart(window: ByteBuffer, mic: Boolean)
    @JvmStatic private external fun nativeStop()
    @JvmStatic private external fun nativeSetMic(allowed: Boolean)
}
