package dev.hyperwin.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.os.storage.StorageManager
import android.os.storage.StorageVolume
import android.provider.Settings
import android.util.Log
import java.nio.ByteBuffer

/**
 * Shares the phone's storage with Windows: internal storage, SD cards and USB drives each show up
 * there as a drive (include/hw_files.h). The requests and file data go through the shared window,
 * served by native code (cpp/files.c). This side only says which volumes to share, and updates
 * that when a card or drive comes or goes.
 *
 * Needs the all-files permission, which the user grants in the system settings.
 */
object FileShare {
    private const val TAG = "HyperWin"
    const val PREF = "share_storage"
    private const val PRIMARY_ID = 1

    init { System.loadLibrary("hyperwin") }

    private var appContext: Context? = null
    private val callback = object : StorageManager.StorageVolumeCallback() {
        override fun onStateChanged(volume: StorageVolume) { appContext?.let { publish(it) } }
    }

    fun permitted(): Boolean = Environment.isExternalStorageManager()

    fun enabled(ctx: Context): Boolean =
        ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).getBoolean(PREF, true)

    /** Opens the system page where the user allows HyperWin to reach all files. */
    fun askPermission(ctx: Context) {
        val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${ctx.packageName}"))
        runCatching { ctx.startActivity(intent) }
            .onFailure { ctx.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)) }
    }

    /** With the window of a VM that is starting. */
    fun start(ctx: Context, window: ByteBuffer) {
        val app = ctx.applicationContext
        nativeStart(window)
        if (appContext == null) {
            app.getSystemService(StorageManager::class.java).registerStorageVolumeCallback(app.mainExecutor, callback)
        }
        appContext = app
        publish(app)
    }

    /** Before the window goes away. */
    fun stop() {
        appContext?.let { ctx ->
            runCatching { ctx.getSystemService(StorageManager::class.java).unregisterStorageVolumeCallback(callback) }
        }
        appContext = null
        nativeStop()
    }

    /** Tells Windows which volumes to show now. */
    fun publish(ctx: Context) {
        if (appContext == null) return
        val volumes = if (enabled(ctx) && permitted()) mounted(ctx) else emptyList()
        Log.i(TAG, "sharing storage: " + volumes.joinToString { "${it.label} at ${it.root}" })
        nativeSetVolumes(
            volumes.map { it.id }.toIntArray(),
            volumes.map { it.flags }.toIntArray(),
            volumes.map { it.label }.toTypedArray(),
            volumes.map { it.root }.toTypedArray(),
        )
    }

    private class Shared(val id: Int, val flags: Int, val label: String, val root: String)

    private fun mounted(ctx: Context): List<Shared> =
        ctx.getSystemService(StorageManager::class.java).storageVolumes.mapNotNull { v ->
            val state = v.state
            if (state != Environment.MEDIA_MOUNTED && state != Environment.MEDIA_MOUNTED_READ_ONLY) return@mapNotNull null
            val root = v.directory?.absolutePath ?: return@mapNotNull null
            val id = if (v.isPrimary) PRIMARY_ID else ((v.uuid ?: root).hashCode() and 0x7fffffff).coerceAtLeast(2)
            var flags = 0
            if (v.isPrimary) flags = flags or 1
            if (v.isRemovable) flags = flags or 2
            if (state == Environment.MEDIA_MOUNTED_READ_ONLY) flags = flags or 4
            val label = if (v.isPrimary) "Phone" else v.getDescription(ctx)
            Shared(id, flags, fit(label), root)
        }.take(8)

    /** At most 31 bytes of UTF-8, cut between characters. */
    private fun fit(label: String): String {
        var s = label
        while (s.toByteArray().size > 31) s = s.dropLast(1)
        return s
    }

    @JvmStatic private external fun nativeStart(window: ByteBuffer)
    @JvmStatic private external fun nativeStop()
    @JvmStatic private external fun nativeSetVolumes(ids: IntArray, flags: IntArray, labels: Array<String>, roots: Array<String>)
}
