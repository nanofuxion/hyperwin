package dev.hyperwin.app

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** One Windows machine: machine.json next to system.img, saved the way Winlator's Container.saveData() saves a profile. */
data class Machine(
    val dir: File,
    var name: String = "Windows",
    var source: String = "",
    var sourceName: String = "",
    var imageIndex: Int = 1,
    var diskSizeGb: Int = DEFAULT_DISK_GB,
    var memoryMb: Int = DEFAULT_MEMORY_MB,
    var cpuCount: Int = 1,
    var screenSize: String = "${AvfPlan.FRAME_WIDTH}x${AvfPlan.FRAME_HEIGHT}",
    var drivers: List<String> = emptyList(),
    var username: String = "User",
    var password: String = "",
    var computerName: String = "HYPERWIN",
    var testSigning: Boolean = true,
    /** Guest uses the phone's own Wi-Fi address instead of a private NAT address. */
    var lanMode: Boolean = false,
    /** Checksum of our boot files as last written to this disk's boot partition. */
    var loaderStamp: Long = 0,
    var prepared: Boolean = false,
) {
    val json get() = File(dir, "machine.json")
    val disk get() = File(dir, "system.img")

    /** Guest frame size from [screenSize], or the default when it is not one the display path takes. */
    fun frameSize(): Pair<Int, Int> {
        val parts = screenSize.split('x').mapNotNull { it.trim().toIntOrNull() }
        return if (parts.size == 2 && AvfPlan.validFrame(parts[0], parts[1])) parts[0] to parts[1]
        else AvfPlan.FRAME_WIDTH to AvfPlan.FRAME_HEIGHT
    }

    fun save() {
        dir.mkdirs()
        val data = JSONObject()
            .put("id", dir.name)
            .put("name", name)
            .put("source", source)
            .put("sourceName", sourceName)
            .put("imageIndex", imageIndex)
            .put("disk", disk.name)
            .put("diskSizeGb", diskSizeGb)
            .put("memoryMb", memoryMb)
            .put("cpuCount", cpuCount)
            .put("screenSize", screenSize)
            .put("drivers", JSONArray(drivers))
            .put("username", username)
            .put("password", password)
            .put("computerName", computerName)
            .put("testSigning", testSigning)
            .put("lanMode", lanMode)
            .put("loaderStamp", loaderStamp)
            .put("prepared", prepared)
        val tmp = File(dir, "machine.json.tmp")
        tmp.writeText(data.toString(2))
        tmp.renameTo(json)
    }

    companion object {
        const val DEFAULT_DISK_GB = 64
        const val MIN_DISK_GB = 24
        const val DEFAULT_MEMORY_MB = 4096
        const val MIN_MEMORY_MB = AvfPlan.WINDOWS_MEMORY_MIB
        private const val ACTIVE = "activeMachine"

        fun root(ctx: Context) = File(ctx.filesDir, "machines")

        fun load(dir: File): Machine? = runCatching {
            val d = JSONObject(File(dir, "machine.json").readText())
            val drivers = d.optJSONArray("drivers")
            Machine(
                dir,
                name = d.optString("name", "Windows"),
                source = d.optString("source"),
                sourceName = d.optString("sourceName"),
                imageIndex = d.optInt("imageIndex", 1),
                diskSizeGb = d.optInt("diskSizeGb", DEFAULT_DISK_GB),
                memoryMb = d.optInt("memoryMb", DEFAULT_MEMORY_MB),
                cpuCount = d.optInt("cpuCount", 1),
                screenSize = d.optString("screenSize", "${AvfPlan.FRAME_WIDTH}x${AvfPlan.FRAME_HEIGHT}"),
                drivers = (0 until (drivers?.length() ?: 0)).map { drivers!!.getString(it) },
                username = d.optString("username", "User"),
                password = d.optString("password"),
                computerName = d.optString("computerName", "HYPERWIN"),
                testSigning = d.optBoolean("testSigning", true),
                lanMode = d.optBoolean("lanMode", false),
                loaderStamp = d.optLong("loaderStamp", 0),
                prepared = d.optBoolean("prepared", false),
            )
        }.getOrNull()

        fun all(ctx: Context): List<Machine> =
            root(ctx).listFiles()?.sortedBy { it.name }?.mapNotNull { load(it) } ?: emptyList()

        fun active(ctx: Context): Machine? {
            val id = prefs(ctx).getString(ACTIVE, null) ?: return null
            return load(File(root(ctx), id))
        }

        fun setActive(ctx: Context, machine: Machine) {
            prefs(ctx).edit().putString(ACTIVE, machine.dir.name).apply()
        }

        fun create(ctx: Context): Machine {
            val root = root(ctx).apply { mkdirs() }
            val id = generateSequence(1) { it + 1 }.first { !File(root, "machine$it").exists() }
            return Machine(File(root, "machine$id"))
        }

        private fun prefs(ctx: Context) = ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE)
    }
}
