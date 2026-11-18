package dev.hyperwin.app

import android.content.Context
import android.net.ConnectivityManager
import android.net.wifi.WifiManager
import android.util.Log
import java.io.InputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger

/**
 * The guest's network. Windows sends raw IPv4 packets (Wintun adapter, carried by [GuestAgent]); this
 * turns them into ordinary Android sockets and back, so no VPN, root or tap device is needed.
 *
 * Two address plans:
 *  - NAT: guest 10.0.2.15, like a home router.
 *  - LAN: the guest carries the phone's own Wi-Fi address. UDP keeps its port numbers, broadcasts go
 *    out for real, and ports Windows programs listen on are opened on the phone, so LAN games see it.
 *
 * 10.0.2.2 is always this app (audio sink) and 10.0.2.3 its DNS forwarder.
 */
class GuestNet(private val ctx: Context, private val lan: Boolean, private val toGuest: (ByteArray, Int) -> Unit) {
    val guestIp: Inet4Address
    private val prefix: Int
    private val gateway: Inet4Address
    private val broadcast: Inet4Address
    val lanActive: Boolean
    private val hostIp = addr(10, 0, 2, 2)
    private val dnsIp = addr(10, 0, 2, 3)
    private val realDns: InetAddress
    private val tcp = ConcurrentHashMap<Long, Tcp>()
    private val udp = ConcurrentHashMap<Int, Udp>()
    private val listeners = ConcurrentHashMap<Int, ServerSocket>()
    private val ipId = AtomicInteger(1)
    private var multicastLock: WifiManager.MulticastLock? = null
    @Volatile private var closed = false

    init {
        val cm = ctx.getSystemService(ConnectivityManager::class.java)
        val link = cm.getLinkProperties(cm.activeNetwork)
        val wifi = if (lan) link?.linkAddresses?.firstOrNull { it.address is Inet4Address && !it.address.isLoopbackAddress } else null
        val route = link?.routes?.firstOrNull { it.isDefaultRoute && it.gateway is Inet4Address }?.gateway as? Inet4Address
        realDns = link?.dnsServers?.firstOrNull { it is Inet4Address } ?: InetAddress.getByName("8.8.8.8")
        if (wifi != null && route != null) {
            guestIp = wifi.address as Inet4Address
            prefix = wifi.prefixLength
            gateway = route
            lanActive = true
            multicastLock = runCatching {
                ctx.applicationContext.getSystemService(WifiManager::class.java).createMulticastLock("hyperwin").apply { acquire() }
            }.getOrNull()
        } else {
            guestIp = addr(10, 0, 2, 15)
            prefix = 24
            gateway = hostIp
            lanActive = false
        }
        val mask = if (prefix == 0) 0 else -1 shl (32 - prefix)
        broadcast = fromInt(toInt(guestIp) or mask.inv())
        Thread({ ticker() }, "hw-net-tick").apply { isDaemon = true; start() }
    }

    /** "ip mask gateway dns mtu" for the guest agent. */
    fun config(): String {
        val mask = fromInt(if (prefix == 0) 0 else -1 shl (32 - prefix))
        return "${guestIp.hostAddress} ${mask.hostAddress} ${gateway.hostAddress} ${dnsIp.hostAddress} $MTU"
    }

    fun close() {
        closed = true
        tcp.values.forEach { it.abort(false) }
        udp.values.forEach { it.close() }
        listeners.values.forEach { runCatching { it.close() } }
        runCatching { multicastLock?.release() }
    }

    // ---- packets from the guest ----

    fun fromGuest(p: ByteArray, len: Int) {
        if (len < 20 || (p[0].toInt() and 0xf0) != 0x40) return
        val ihl = (p[0].toInt() and 15) * 4
        val total = minOf(u16(p, 2), len)
        if (ihl < 20 || total < ihl || (u16(p, 6) and 0x3fff) != 0) return   // no fragments
        val dst = InetAddress.getByAddress(p.copyOfRange(16, 20)) as Inet4Address
        when (p[9].toInt()) {
            6 -> if (total >= ihl + 20) tcpIn(p, ihl, total, dst)
            17 -> if (total >= ihl + 8) udpIn(p, ihl, total, dst)
            1 -> if (total >= ihl + 8 && p[ihl].toInt() == 8) ping(p.copyOf(total), ihl, dst)
        }
    }

    private fun ping(p: ByteArray, ihl: Int, dst: Inet4Address) {
        Thread {
            if (dst != hostIp && dst != dnsIp && !runCatching { dst.isReachable(2000) }.getOrDefault(false)) return@Thread
            val out = p.copyOf()
            System.arraycopy(p, 16, out, 12, 4)
            System.arraycopy(p, 12, out, 16, 4)
            out[ihl] = 0
            put16(out, ihl + 2, 0)
            put16(out, ihl + 2, checksum(out, ihl, out.size - ihl, 0))
            put16(out, 10, 0)
            put16(out, 10, checksum(out, 0, ihl, 0))
            send(out)
        }.apply { isDaemon = true; start() }
    }

    private fun send(packet: ByteArray) {
        if (!closed) toGuest(packet, packet.size)
    }

    private fun ipHeader(proto: Int, src: InetAddress, payloadLen: Int): ByteArray {
        val p = ByteArray(20 + payloadLen)
        p[0] = 0x45
        put16(p, 2, p.size)
        put16(p, 4, ipId.getAndIncrement() and 0xffff)
        p[8] = 64
        p[9] = proto.toByte()
        System.arraycopy(src.address, 0, p, 12, 4)
        System.arraycopy(guestIp.address, 0, p, 16, 4)
        put16(p, 10, checksum(p, 0, 20, 0))
        return p
    }

    private fun l4Checksum(p: ByteArray, proto: Int) {
        val l4 = p.size - 20
        var sum = proto + l4
        for (i in 12 until 20 step 2) sum += u16(p, i)
        put16(p, if (proto == 6) 36 else 26, checksum(p, 20, l4, sum).let { if (proto == 17 && it == 0) 0xffff else it })
    }

    // ---- UDP ----

    private inner class Udp(val guestPort: Int, val pinned: Boolean) {
        val socket: DatagramSocket = DatagramSocket(null).apply {
            reuseAddress = true
            // LAN mode keeps the guest's port so replies and broadcasts addressed to it arrive here.
            if (!lanActive || runCatching { bind(InetSocketAddress(guestPort)) }.isFailure) bind(null)
            this.broadcast = true
        }
        @Volatile var lastUse = System.nanoTime()

        init {
            Thread({
                val buf = ByteArray(65535)
                val dp = DatagramPacket(buf, buf.size)
                while (!socket.isClosed) {
                    try { socket.receive(dp) } catch (e: Exception) { break }
                    lastUse = System.nanoTime()
                    val from = if (dp.address == realDns && dp.port == 53) dnsIp else dp.address
                    if (from !is Inet4Address || (from == guestIp && dp.port == guestPort)) continue   // our own broadcast
                    if (dp.length + 28 > MTU) continue
                    val p = ipHeader(17, from, 8 + dp.length)
                    put16(p, 20, dp.port)
                    put16(p, 22, guestPort)
                    put16(p, 24, 8 + dp.length)
                    System.arraycopy(buf, 0, p, 28, dp.length)
                    l4Checksum(p, 17)
                    send(p)
                }
            }, "hw-udp-$guestPort").apply { isDaemon = true; start() }
        }

        fun close() {
            socket.close()
            udp.remove(guestPort, this)
        }
    }

    private fun udpIn(p: ByteArray, ihl: Int, total: Int, dst: Inet4Address) {
        val sport = u16(p, ihl)
        val dport = u16(p, ihl + 2)
        val n = minOf(u16(p, ihl + 4), total - ihl) - 8
        if (n < 0) return
        if (dst == hostIp) return
        val target: InetAddress = when {
            dst == dnsIp -> realDns
            dst == broadcast || dst == LIMITED_BROADCAST -> if (lanActive) broadcast else return
            dst.isMulticastAddress -> return
            else -> dst
        }
        val flow = udp[sport] ?: runCatching { Udp(sport, false) }.getOrNull()?.also { udp[sport] = it } ?: return
        flow.lastUse = System.nanoTime()
        runCatching { flow.socket.send(DatagramPacket(p, ihl + 8, n, target, dport)) }
    }

    // ---- TCP ----

    private fun key(guestPort: Int, remote: InetAddress, remotePort: Int) =
        (guestPort.toLong() shl 48) or (remotePort.toLong() shl 32) or (toInt(remote).toLong() and 0xffffffffL)

    /**
     * One guest TCP connection ended here and continued on a real socket. The link to the guest
     * neither reorders nor (normally) drops, so this keeps only what correctness needs: sequence
     * tracking, the guest's receive window, and a slow resend of unacknowledged data.
     */
    private inner class Tcp(val guestPort: Int, val remote: InetAddress, val remotePort: Int, val inbound: Boolean) {
        val id = key(guestPort, remote, remotePort)
        val lock = Object()
        var socket: Socket? = null
        var established = false
        var dead = false
        var rcvNxt = 0
        var sndNxt = (System.nanoTime() / 1000).toInt()
        var sndUna = sndNxt
        var guestWnd = 65535
        var mss = MTU - 40
        var guestFin = false
        var ourFin = false
        var lastProgress = System.nanoTime()
        val unacked = ArrayDeque<Pair<Int, ByteArray>>()   // (seq, packet) awaiting the guest's ACK
        val toRemote = LinkedBlockingQueue<ByteArray>()
        val queued = AtomicInteger(0)

        fun window() = (65535 - queued.get()).coerceIn(0, 65535)

        /** Call with [lock] held. */
        fun segment(flags: Int, data: ByteArray? = null, len: Int = 0, synMss: Boolean = false) {
            val opt = if (synMss) 4 else 0
            val p = ipHeader(6, remote, 20 + opt + len)
            put16(p, 20, remotePort)
            put16(p, 22, guestPort)
            put32(p, 24, sndNxt)
            put32(p, 28, if (flags and ACK != 0) rcvNxt else 0)
            p[32] = (((20 + opt) / 4) shl 4).toByte()
            p[33] = flags.toByte()
            put16(p, 34, window())
            if (synMss) { p[40] = 2; p[41] = 4; put16(p, 42, MTU - 40) }
            if (len > 0) System.arraycopy(data!!, 0, p, 40 + opt, len)
            l4Checksum(p, 6)
            val consumed = len + (if (flags and (SYN or FIN) != 0) 1 else 0)
            if (consumed > 0) { unacked.addLast(sndNxt to p); sndNxt += consumed }
            send(p)
        }

        fun abort(reset: Boolean) {
            synchronized(lock) {
                if (dead) return
                if (reset) segment(RST or ACK)
                dead = true
                lock.notifyAll()
            }
            tcp.remove(id, this)
            runCatching { socket?.close() }
            toRemote.offer(CLOSE)
        }

        fun finishIfDone() {
            if (guestFin && ourFin && unacked.isEmpty()) {
                dead = true
                tcp.remove(id, this)
                runCatching { socket?.close() }
                toRemote.offer(CLOSE)
            }
        }

        fun startPumps() {
            val s = socket ?: return
            Thread({ pumpFromRemote(s.getInputStream()) }, "hw-tcp-r").apply { isDaemon = true; start() }
            Thread({
                val out = s.getOutputStream()
                try {
                    while (true) {
                        val chunk = toRemote.take()
                        if (chunk === CLOSE) break
                        if (chunk === EOF) { s.shutdownOutput(); continue }
                        out.write(chunk)
                        val before = window()
                        queued.addAndGet(-chunk.size)
                        if (before < mss) synchronized(lock) { if (!dead) segment(ACK) }   // reopen the window
                    }
                } catch (e: Exception) {
                    abort(true)
                }
            }, "hw-tcp-w").apply { isDaemon = true; start() }
        }

        private fun pumpFromRemote(input: InputStream) {
            val buf = ByteArray(mss.coerceAtMost(32768))
            try {
                while (true) {
                    var room: Int
                    synchronized(lock) {
                        while (true) {
                            if (dead) return
                            room = guestWnd - (sndNxt - sndUna)
                            if (room > 0) break
                            lock.wait(500)
                        }
                    }
                    val n = input.read(buf, 0, minOf(buf.size, room))
                    synchronized(lock) {
                        if (dead) return
                        if (n < 0) {
                            segment(FIN or ACK)
                            ourFin = true
                            finishIfDone()
                            return
                        }
                        segment(PSH or ACK, buf, n)
                    }
                }
            } catch (e: Exception) {
                abort(true)
            }
        }

        fun fromGuest(p: ByteArray, off: Int, total: Int) {
            val flags = p[off + 13].toInt() and 0xff
            val seq = i32(p, off + 4)
            val ack = i32(p, off + 8)
            val hdr = ((p[off + 12].toInt() and 0xf0) shr 4) * 4
            val len = total - off - hdr
            if (len < 0) return
            if (flags and RST != 0) { abort(false); return }
            synchronized(lock) {
                if (dead) return
                if (flags and SYN != 0) {
                    if (inbound && flags and ACK != 0 && !established) {      // guest accepted our SYN
                        rcvNxt = seq + 1
                        mss = minOf(mss, synMss(p, off, hdr))
                        acked(ack)
                        established = true
                        segment(ACK)
                        startPumps()
                    } else if (!inbound && established) {
                        unacked.firstOrNull()?.let { send(it.second) }       // our SYN-ACK was lost
                    }
                    return
                }
                if (!established) return
                guestWnd = u16(p, off + 14)
                if (flags and ACK != 0) acked(ack)
                var skip = rcvNxt - seq
                if (len > 0 || flags and FIN != 0) {
                    if (skip < 0 || skip > len) { segment(ACK); return }     // gap or stale: say what we expect
                    if (len - skip > 0 && !guestFin) {
                        val take = minOf(len - skip, window())
                        if (take > 0) {
                            toRemote.offer(p.copyOfRange(off + hdr + skip, off + hdr + skip + take))
                            queued.addAndGet(take)
                            rcvNxt += take
                        }
                        if (take < len - skip) { segment(ACK); return }      // window full, drop the rest
                    }
                    if (flags and FIN != 0 && !guestFin) {
                        guestFin = true
                        rcvNxt += 1
                        toRemote.offer(EOF)
                    }
                    segment(ACK)
                }
                lock.notifyAll()
                finishIfDone()
            }
        }

        private fun acked(ack: Int) {
            if (ack - sndUna > 0 && ack - sndNxt <= 0) {
                sndUna = ack
                lastProgress = System.nanoTime()
                while (unacked.isNotEmpty()) {
                    val (seq, pkt) = unacked.first()
                    val end = seq + tcpLength(pkt)
                    if (end - ack <= 0) unacked.removeFirst() else break
                }
            }
        }

        /** Resend the oldest unacknowledged segment if the guest has been silent about it. */
        fun tick(now: Long) {
            synchronized(lock) {
                if (dead || unacked.isEmpty() || now - lastProgress < 1_000_000_000L) return
                lastProgress = now
                send(unacked.first().second)
            }
        }
    }

    private fun tcpLength(pkt: ByteArray): Int {
        val hdr = ((pkt[32].toInt() and 0xf0) shr 4) * 4
        val flags = pkt[33].toInt()
        return pkt.size - 20 - hdr + (if (flags and (SYN or FIN) != 0) 1 else 0)
    }

    private fun synMss(p: ByteArray, off: Int, hdr: Int): Int {
        var i = off + 20
        while (i + 1 < off + hdr) {
            when (p[i].toInt()) {
                0 -> return MTU - 40
                1 -> i += 1
                2 -> return u16(p, i + 2)
                else -> i += maxOf(2, p[i + 1].toInt() and 0xff)
            }
        }
        return 536
    }

    private fun tcpIn(p: ByteArray, ihl: Int, total: Int, dst: Inet4Address) {
        val sport = u16(p, ihl)
        val dport = u16(p, ihl + 2)
        val flags = p[ihl + 13].toInt() and 0xff
        val id = key(sport, dst, dport)
        tcp[id]?.let { it.fromGuest(p, ihl, total); return }
        if (flags and RST != 0) return
        if (flags and (SYN or ACK) != SYN) {
            if (total - ihl > 20 || flags and FIN != 0) reset(p, ihl, total, dst)   // unknown connection
            return
        }
        val conn = Tcp(sport, dst, dport, inbound = false)
        conn.rcvNxt = i32(p, ihl + 4) + 1
        conn.mss = minOf(conn.mss, synMss(p, ihl, ((p[ihl + 12].toInt() and 0xf0) shr 4) * 4))
        conn.guestWnd = u16(p, ihl + 14)
        tcp[id] = conn
        Thread({
            val s = Socket()
            val ok = runCatching {
                s.tcpNoDelay = true
                s.connect(InetSocketAddress(dst, dport), 10_000)
            }.isSuccess
            synchronized(conn.lock) {
                if (conn.dead) { runCatching { s.close() }; return@Thread }
                if (ok) {
                    conn.socket = s
                    conn.established = true
                    conn.segment(SYN or ACK, synMss = true)
                    conn.startPumps()
                }
            }
            if (!ok) { runCatching { s.close() }; conn.abort(true) }
        }, "hw-tcp-c").apply { isDaemon = true; start() }
    }

    private fun reset(p: ByteArray, ihl: Int, total: Int, dst: Inet4Address) {
        val out = ipHeader(6, dst, 20)
        put16(out, 20, u16(p, ihl + 2))
        put16(out, 22, u16(p, ihl))
        put32(out, 24, i32(p, ihl + 8))
        out[32] = 0x50
        out[33] = RST.toByte()
        l4Checksum(out, 6)
        send(out)
    }

    // ---- LAN mode: accept traffic for the ports Windows programs opened ----

    /** Called with the guest's listening TCP ports and bound UDP ports. */
    fun mirrorPorts(tcpPorts: Set<Int>, udpPorts: Set<Int>) {
        if (!lanActive || closed) return
        for (port in tcpPorts) {
            if (port < 1024 || listeners.containsKey(port)) continue
            val server = runCatching { ServerSocket().apply { reuseAddress = true; bind(InetSocketAddress(port)) } }.getOrNull() ?: continue
            listeners[port] = server
            Thread({ acceptLoop(server, port) }, "hw-listen-$port").apply { isDaemon = true; start() }
        }
        listeners.keys.filter { it !in tcpPorts }.forEach { listeners.remove(it)?.let { s -> runCatching { s.close() } } }
        for (port in udpPorts) {
            if (port < 1024 || udp.containsKey(port)) continue
            runCatching { Udp(port, true) }.getOrNull()?.let { udp[port] = it }
        }
        udp.values.filter { it.pinned && it.guestPort !in udpPorts }.forEach { it.close() }
    }

    private fun acceptLoop(server: ServerSocket, port: Int) {
        while (!server.isClosed) {
            val s = try { server.accept() } catch (e: Exception) { break }
            val peer = s.inetAddress
            if (peer !is Inet4Address) { runCatching { s.close() }; continue }
            val conn = Tcp(port, peer, s.port, inbound = true)
            conn.socket = s
            runCatching { s.tcpNoDelay = true }
            tcp[conn.id] = conn
            synchronized(conn.lock) { conn.segment(SYN, synMss = true) }
        }
    }

    private fun ticker() {
        while (!closed) {
            Thread.sleep(500)
            val now = System.nanoTime()
            tcp.values.forEach { c ->
                c.tick(now)
                // An inbound connection the guest never answered (firewall) is dropped after a while.
                if (c.inbound && !c.established && now - c.lastProgress > 10_000_000_000L) c.abort(false)
            }
            udp.values.forEach { if (!it.pinned && now - it.lastUse > 120_000_000_000L) it.close() }
        }
    }

    companion object {
        const val MTU = 16384
        private const val FIN = 1
        private const val SYN = 2
        private const val RST = 4
        private const val PSH = 8
        private const val ACK = 16
        private val EOF = ByteArray(0)
        private val CLOSE = ByteArray(0)
        private val LIMITED_BROADCAST = addr(255, 255, 255, 255)

        private fun addr(a: Int, b: Int, c: Int, d: Int) =
            InetAddress.getByAddress(byteArrayOf(a.toByte(), b.toByte(), c.toByte(), d.toByte())) as Inet4Address
        private fun toInt(a: InetAddress) = i32(a.address, 0)
        private fun fromInt(v: Int) = addr(v ushr 24, (v shr 16) and 255, (v shr 8) and 255, v and 255)
        private fun u16(p: ByteArray, o: Int) = ((p[o].toInt() and 0xff) shl 8) or (p[o + 1].toInt() and 0xff)
        private fun i32(p: ByteArray, o: Int) = (u16(p, o) shl 16) or u16(p, o + 2)
        private fun put16(p: ByteArray, o: Int, v: Int) { p[o] = (v shr 8).toByte(); p[o + 1] = v.toByte() }
        private fun put32(p: ByteArray, o: Int, v: Int) { put16(p, o, v ushr 16); put16(p, o + 2, v and 0xffff) }
        private fun checksum(p: ByteArray, off: Int, len: Int, seed: Int): Int {
            var sum = seed.toLong()
            var i = off
            while (i + 1 < off + len) { sum += u16(p, i); i += 2 }
            if (i < off + len) sum += (p[i].toInt() and 0xff) shl 8
            while (sum shr 16 != 0L) sum = (sum and 0xffff) + (sum shr 16)
            return sum.toInt().inv() and 0xffff
        }
    }
}
