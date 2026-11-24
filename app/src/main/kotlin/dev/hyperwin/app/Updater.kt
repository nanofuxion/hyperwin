package dev.hyperwin.app

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.view.WindowManager
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/** App updates from GitHub Releases (updates/README.md). Release builds only. */
object Updater {
    private const val LATEST = "https://api.github.com/repos/nanofuxion/hyperwin/releases/latest"
    private const val CHECKED_AT = "updateCheckedAt"
    private const val DECLINED = "updateDeclined"
    private const val DAY_MS = 24 * 60 * 60 * 1000L
    private const val INSTALL_RESULT = "dev.hyperwin.app.UPDATE_RESULT"

    private class Release(val version: String, val notes: String, val page: String, val apk: String, val size: Long)

    @Volatile private var busy = false

    /** Debug builds are signed with another key and are built from source, so they never update this way. */
    fun enabled(ctx: Context) = ctx.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE == 0

    fun current(ctx: Context): String = ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "0"

    /** [manual] is the Settings button: it always checks, says what it found, and offers a declined version again. */
    fun check(activity: Activity, manual: Boolean, mayAsk: () -> Boolean = { true }) {
        if (!enabled(activity) || busy) return
        val prefs = activity.getSharedPreferences("hyperwin", Context.MODE_PRIVATE)
        val now = System.currentTimeMillis()
        if (!manual && now - prefs.getLong(CHECKED_AT, 0) < DAY_MS) return
        busy = true
        Thread {
            val found = runCatching { latest() }
            busy = false
            if (found.isSuccess) prefs.edit().putLong(CHECKED_AT, now).apply()
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed || !mayAsk()) return@runOnUiThread
                val release = found.getOrNull()
                when {
                    found.isFailure -> if (manual) say(activity, "Could not check for updates", found.exceptionOrNull()?.message ?: "No answer from GitHub.")
                    release == null || !newer(release.version, current(activity)) ->
                        if (manual) say(activity, "You're up to date", "HyperWin ${current(activity)} is the newest version.")
                    !manual && release.version == prefs.getString(DECLINED, null) -> Unit
                    else -> offer(activity, release)
                }
            }
        }.start()
    }

    /** The newest full release, or null when it has no APK or a tag that is not dotted numbers. */
    private fun latest(): Release? {
        val http = URL(LATEST).openConnection() as HttpURLConnection
        http.setRequestProperty("Accept", "application/vnd.github+json")
        http.connectTimeout = 10_000
        http.readTimeout = 10_000
        try {
            if (http.responseCode == 404) return null       // no release yet
            if (http.responseCode != 200) error("GitHub answered ${http.responseCode}.")
            val json = JSONObject(http.inputStream.bufferedReader().use { it.readText() })
            val version = json.getString("tag_name").removePrefix("v")
            if (parts(version) == null) return null
            val assets = json.getJSONArray("assets")
            val apk = (0 until assets.length()).map { assets.getJSONObject(it) }
                .firstOrNull { it.getString("name").endsWith(".apk") } ?: return null
            return Release(version, json.optString("body"), json.getString("html_url"),
                apk.getString("browser_download_url"), apk.getLong("size"))
        } finally {
            http.disconnect()
        }
    }

    private fun parts(version: String): List<Int>? = version.split('.').map { it.toIntOrNull() ?: return null }

    /** Dotted numbers: 0.1.10 beats 0.1.9, and 0.1.1 beats 0.1. Anything that does not parse is not newer. */
    internal fun newer(candidate: String, installed: String): Boolean {
        val a = parts(candidate.removePrefix("v")) ?: return false
        val b = parts(installed.removePrefix("v")) ?: return false
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }
            val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }

    private fun say(activity: Activity, title: String, message: String) {
        MaterialAlertDialogBuilder(activity).setTitle(title).setMessage(message).setPositiveButton("OK", null).show()
    }

    private fun offer(activity: Activity, release: Release) {
        val running = AvfLaunch.machine != null
        val notes = release.notes.trim().let { if (it.length > 600) it.take(600).trimEnd() + "…" else it }
        val warning = if (running) "Installing closes HyperWin, so Windows shuts down first." else ""
        MaterialAlertDialogBuilder(activity)
            .setTitle("HyperWin ${release.version} is out")
            .setMessage(listOf(notes, warning).filter { it.isNotEmpty() }.joinToString("\n\n"))
            .setNeutralButton("See on GitHub") { _, _ ->
                runCatching { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(release.page))) }
            }
            .setNegativeButton("Not now") { _, _ ->
                activity.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).edit().putString(DECLINED, release.version).apply()
            }
            .setPositiveButton(if (running) "Shut down and update" else "Update") { _, _ -> update(activity, release) }
            .show()
    }

    private fun update(activity: Activity, release: Release) {
        val progress = MaterialAlertDialogBuilder(activity)
            .setTitle("Updating HyperWin")
            .setMessage("Starting.")
            .setCancelable(false)
            .show()
        activity.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        fun step(text: String) = activity.runOnUiThread { progress.setMessage(text) }
        Thread {
            val apk = File(activity.cacheDir, "update.apk")
            val failure = runCatching {
                // Installing kills this process, and the virtual machine with it, so Windows must be off first.
                if (AvfLaunch.machine != null) {
                    step("Waiting for Windows to shut down.")
                    activity.runOnUiThread { (activity as? HomeScreen.Actions)?.shutDown() }
                    val deadline = System.currentTimeMillis() + 180_000
                    while (AvfLaunch.machine != null) {
                        if (System.currentTimeMillis() > deadline) error("Windows did not shut down. Shut it down, then try again.")
                        Thread.sleep(500)
                    }
                }
                download(release, apk) { step("Downloading. $it%") }
                step("Checking the download.")
                verify(activity, release, apk)
                step("Android will ask you to confirm.")
                install(activity, apk, progress)
            }.exceptionOrNull()
            apk.delete()
            if (failure != null) activity.runOnUiThread {
                done(activity, progress)
                if (!activity.isFinishing && !activity.isDestroyed) say(activity, "The update did not install", failure.message ?: "Something went wrong.")
            }
        }.start()
    }

    private fun done(activity: Activity, progress: AlertDialog) {
        runCatching { progress.dismiss() }
        // The Windows screen holds the screen on by itself, and the update dialogs never show over it.
        if (AvfLaunch.machine == null) activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun download(release: Release, to: File, percent: (Int) -> Unit) {
        val http = URL(release.apk).openConnection() as HttpURLConnection
        http.connectTimeout = 15_000
        http.readTimeout = 30_000
        try {
            if (http.responseCode != 200) error("The download failed: GitHub answered ${http.responseCode}.")
            http.inputStream.use { input ->
                to.outputStream().use { output ->
                    val buffer = ByteArray(1 shl 16)
                    var total = 0L
                    var shown = -1
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        output.write(buffer, 0, n)
                        total += n
                        val now = (total * 100 / release.size.coerceAtLeast(1)).toInt().coerceAtMost(100)
                        if (now != shown) { shown = now; percent(now) }
                    }
                }
            }
        } finally {
            http.disconnect()
        }
    }

    /** Android refuses a wrong package or signature anyway; this gives a clear message before it gets that far. */
    private fun verify(ctx: Context, release: Release, apk: File) {
        if (apk.length() != release.size) error("The download is incomplete. Try again.")
        val pm = ctx.packageManager
        val info = pm.getPackageArchiveInfo(apk.path, PackageManager.GET_SIGNING_CERTIFICATES)
            ?: error("The downloaded file is not an Android app.")
        if (info.packageName != ctx.packageName) error("The downloaded file is not HyperWin.")
        fun certs(signing: android.content.pm.SigningInfo?) =
            signing?.let { if (it.hasMultipleSigners()) it.apkContentsSigners else it.signingCertificateHistory }
                ?.toSet()
        val theirs = certs(info.signingInfo) ?: return      // some Android versions do not read it from a file
        val ours = certs(pm.getPackageInfo(ctx.packageName, PackageManager.GET_SIGNING_CERTIFICATES).signingInfo) ?: return
        if (theirs.intersect(ours).isEmpty()) error("The download is not signed by HyperWin, so it was not installed.")
    }

    private fun install(activity: Activity, apk: File, progress: AlertDialog) {
        val app = activity.applicationContext
        val installer = app.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(app.packageName)
            setSize(apk.length())
        }
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            session.openWrite("hyperwin.apk", 0, apk.length()).use { out ->
                apk.inputStream().use { it.copyTo(out) }
                session.fsync(out)
            }
            app.registerReceiver(object : BroadcastReceiver() {
                override fun onReceive(ctx: Context, intent: Intent) {
                    if (intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1) != id) return
                    when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                        PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                            // Android's own confirm screen. The first time, it also asks to allow installs from HyperWin.
                            val confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java) ?: return
                            runCatching { activity.startActivity(confirm) }
                            return
                        }
                        PackageInstaller.STATUS_SUCCESS -> Unit      // the app is replaced; this process is ending
                        else -> activity.runOnUiThread {
                            done(activity, progress)
                            val why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
                            if (status != PackageInstaller.STATUS_FAILURE_ABORTED && !activity.isFinishing && !activity.isDestroyed)
                                say(activity, "The update did not install", why ?: "Android refused it.")
                        }
                    }
                    runCatching { app.unregisterReceiver(this) }
                }
            }, IntentFilter(INSTALL_RESULT), Context.RECEIVER_NOT_EXPORTED)
            val result = PendingIntent.getBroadcast(app, id, Intent(INSTALL_RESULT).setPackage(app.packageName),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE)
            session.commit(result.intentSender)
        }
    }
}
