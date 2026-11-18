package dev.hyperwin.app

import java.nio.ByteBuffer

object WindowBridge {
    init { System.loadLibrary("hyperwin") }

    external fun nativeInit(buffer: ByteBuffer, seed: ByteArray)
    external fun nativeConfigure(
        buffer: ByteBuffer,
        width: Int,
        height: Int,
        cpuMhz: Int,
        systemMaker: String,
        systemModel: String,
        cpuMaker: String,
        cpuName: String,
    )
    external fun nativePoll(buffer: ByteBuffer): Int
    external fun nativeCopyFrame(buffer: ByteBuffer, frame: ByteBuffer): Int
    /** Where [nativePresent] draws. Null stops drawing and waits for a draw in flight. */
    external fun nativeSetSurface(surface: android.view.Surface?)
    /** Shows the guest frame if it changed: 1 drew, 0 unchanged, -1 nothing to show yet. */
    external fun nativePresent(buffer: ByteBuffer): Int
}
