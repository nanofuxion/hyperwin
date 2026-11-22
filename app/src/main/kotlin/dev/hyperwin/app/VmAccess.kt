package dev.hyperwin.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import rikka.shizuku.Shizuku

/**
 * Android only lets a shell grant the two permissions that start a virtual machine (both are
 * signature|development). Shizuku is a small app that holds that shell once started from Wireless
 * debugging, so this asks it to run `pm grant` once. The adb command is the fallback.
 */
object VmAccess {
    private const val SHIZUKU = "moe.shizuku.privileged.api"
    private const val SHIZUKU_PAGE = "https://shizuku.rikka.app/download/"
    private val PERMS = listOf(
        "android.permission.MANAGE_VIRTUAL_MACHINE",
        "android.permission.USE_CUSTOM_VIRTUAL_MACHINE",
    )

    fun granted(a: Activity) = PERMS.all { a.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    private fun adbCommand(a: Activity) = PERMS.joinToString(" && ") { "adb shell pm grant ${a.packageName} $it" }
    private fun installed(a: Activity) = runCatching { a.packageManager.getPackageInfo(SHIZUKU, 0) }.isSuccess
    private fun running() = runCatching { Shizuku.pingBinder() }.getOrDefault(false)

    /** Runs [then] at once when the app may start virtual machines, else walks the user through the grant. */
    fun ensure(a: Activity, then: () -> Unit) {
        if (granted(a)) then() else ask(a)
    }

    private fun ask(a: Activity) {
        val state = when { !installed(a) -> 0; !running() -> 1; else -> 2 }
        val steps = "Windows runs in a virtual machine, and Android only lets a computer or a helper app switch that on for an app. This is a one-time step.\n\n" + when (state) {
            0 -> "The easy way is Shizuku. Install it, start it from Wireless debugging, then come back here."
            1 -> "Shizuku is installed but not running. Open it, start it, then come back here."
            else -> "Shizuku is ready. Tap Allow to switch it on."
        } + "\n\nNo Shizuku? Copy the adb command and run it from a computer once."
        MaterialAlertDialogBuilder(a)
            .setTitle("Allow virtual machines")
            .setMessage(steps)
            .setPositiveButton(when (state) { 0 -> "Get Shizuku"; 1 -> "Open Shizuku"; else -> "Allow" }) { _, _ ->
                when (state) {
                    0 -> a.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(SHIZUKU_PAGE)))
                    1 -> a.packageManager.getLaunchIntentForPackage(SHIZUKU)?.let { a.startActivity(it) }
                    else -> grant(a)
                }
            }
            .setNeutralButton("Copy adb command") { _, _ ->
                (a.getSystemService(ClipboardManager::class.java)).setPrimaryClip(ClipData.newPlainText("adb", adbCommand(a)))
                Toast.makeText(a, "Copied. Run it once, then start Windows again.", Toast.LENGTH_LONG).show()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun grant(a: Activity) {
        if (Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) return runGrants(a)
        Shizuku.addRequestPermissionResultListener(object : Shizuku.OnRequestPermissionResultListener {
            override fun onRequestPermissionResult(requestCode: Int, grantResult: Int) {
                Shizuku.removeRequestPermissionResultListener(this)
                a.runOnUiThread {
                    if (grantResult == PackageManager.PERMISSION_GRANTED) runGrants(a)
                    else Toast.makeText(a, "Shizuku was not allowed to help.", Toast.LENGTH_LONG).show()
                }
            }
        })
        Shizuku.requestPermission(1)
    }

    private fun runGrants(a: Activity) {
        Thread {
            val failure = PERMS.firstNotNullOfOrNull { perm -> shell(arrayOf("pm", "grant", a.packageName, perm)) }
            a.runOnUiThread {
                if (failure != null) {
                    MaterialAlertDialogBuilder(a).setTitle("It did not work")
                        .setMessage("This phone may not support virtual machines.\n\n$failure").setPositiveButton("OK", null).show()
                } else {
                    // The group id that opens the VM service only reaches a new process.
                    Toast.makeText(a, "Done. Restarting the app.", Toast.LENGTH_SHORT).show()
                    a.startActivity(a.packageManager.getLaunchIntentForPackage(a.packageName)!!
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK))
                    android.os.Process.killProcess(android.os.Process.myPid())
                }
            }
        }.start()
    }

    /** Runs a command as Shizuku's shell. Null when it worked, else what went wrong. */
    private fun shell(cmd: Array<String>): String? = runCatching {
        // Shizuku keeps newProcess private since version 13, but the call still works.
        val m = Shizuku::class.java.getDeclaredMethod("newProcess", Array<String>::class.java, Array<String>::class.java, String::class.java)
            .apply { isAccessible = true }
        val p = m.invoke(null, cmd, null, null) as Process
        val err = p.errorStream.bufferedReader().readText().trim()
        if (p.waitFor() == 0) null else err.ifEmpty { "exit ${p.exitValue()}" }
    }.getOrElse { it.message ?: it.toString() }
}
