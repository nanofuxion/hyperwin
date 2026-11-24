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
    external fun nativeConfigureCpuTopology(
        buffer: ByteBuffer,
        maxMhz: IntArray,
        minMhz: IntArray,
        capacity: IntArray,
        effClass: ByteArray,
    )
    /** The Windows screen size for the indirect display: the app's choice and the phone's own size. */
    external fun nativeSetScreen(buffer: ByteBuffer, width: Int, height: Int, nativeWidth: Int, nativeHeight: Int, ask: Boolean)
    external fun nativePoll(buffer: ByteBuffer): Int
    external fun nativeCopyFrame(buffer: ByteBuffer, frame: ByteBuffer): Int
    /** Where [nativePresent] draws. Null stops drawing and waits for a draw in flight. */
    external fun nativeSetSurface(surface: android.view.Surface?)
    /** Shows the guest frame if it changed: 1 drew, 0 unchanged, -1 nothing to show yet. */
    external fun nativePresent(buffer: ByteBuffer): Int
    /** The brightest colour channel in the guest frame (0..255), or -1 when there is none. */
    external fun nativeFramePeak(buffer: ByteBuffer): Int
}
