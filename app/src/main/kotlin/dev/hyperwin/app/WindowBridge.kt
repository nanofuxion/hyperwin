package dev.hyperwin.app

import java.nio.ByteBuffer

object WindowBridge {
    init { System.loadLibrary("hyperwin") }

    external fun nativeInit(buffer: ByteBuffer, seed: ByteArray)
    external fun nativePoll(buffer: ByteBuffer): Int
    external fun nativeCopyFrame(buffer: ByteBuffer, frame: ByteBuffer): Int
    external fun nativePad(
        buffer: ByteBuffer,
        connected: Int,
        packet: Int,
        buttons: Int,
        lt: Int,
        rt: Int,
        lx: Int,
        ly: Int,
        rx: Int,
        ry: Int,
    )
}
