package dev.hyperwin.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.util.concurrent.ArrayBlockingQueue

/**
 * Plays the guest's sound. Windows uses the Scream virtual sound card, which sends raw PCM in UDP
 * packets to 10.0.2.2:4010; [GuestNet] hands those here. Packet: rate code, bits per sample,
 * channels, channel mask (2 bytes), then PCM.
 */
object ScreamAudio {
    const val PORT = 4010
    private val queue = ArrayBlockingQueue<ByteArray>(64)
    private var track: AudioTrack? = null
    private var format = 0
    private var thread: Thread? = null

    fun packet(p: ByteArray, off: Int, len: Int) {
        if (len <= 5) return
        if (thread == null) synchronized(this) {
            if (thread == null) thread = Thread({ play() }, "hw-audio").apply { isDaemon = true; start() }
        }
        if (!queue.offer(p.copyOfRange(off, off + len))) { queue.poll(); queue.offer(p.copyOfRange(off, off + len)) }
    }

    fun stop() {
        queue.clear()
        synchronized(this) {
            runCatching { track?.release() }
            track = null
            format = 0
        }
    }

    private fun play() {
        while (true) {
            val p = queue.take()
            val rateCode = p[0].toInt() and 0xff
            val bits = p[1].toInt() and 0xff
            val channels = p[2].toInt() and 0xff
            val rate = (if (rateCode and 0x80 != 0) 44100 else 48000) * (rateCode and 0x7f).coerceAtLeast(1)
            if (channels !in 1..2 || bits !in intArrayOf(16, 24, 32)) continue
            val key = rate * 1000 + bits * 10 + channels
            val t = synchronized(this) {
                if (key != format) {
                    runCatching { track?.release() }
                    track = runCatching { open(rate, channels) }.onFailure { Log.e("HyperWin", "audio open failed", it) }.getOrNull()
                    format = key
                    Log.i("HyperWin", "guest audio $rate Hz, $bits bit, $channels ch")
                }
                track
            } ?: continue
            val pcm = if (bits == 16) p.copyOfRange(5, p.size) else to16(p, bits / 8)
            t.write(pcm, 0, pcm.size)
        }
    }

    private fun to16(p: ByteArray, width: Int): ByteArray {
        val samples = (p.size - 5) / width
        val out = ByteArray(samples * 2)
        for (i in 0 until samples) {
            out[2 * i] = p[5 + i * width + width - 2]
            out[2 * i + 1] = p[5 + i * width + width - 1]
        }
        return out
    }

    private fun open(rate: Int, channels: Int): AudioTrack {
        val mask = if (channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val min = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        return AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(rate).setChannelMask(mask)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(maxOf(min, rate * channels * 2 / 10))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build().apply { play() }
    }
}
