package dev.hyperwin.app

import android.app.Application
import android.content.Context
import android.util.Log
import org.lsposed.hiddenapibypass.HiddenApiBypass

class HyperWinApp : Application() {
    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(base)
        runCatching {
            HiddenApiBypass.addHiddenApiExemptions(
                "Landroid/system/virtualmachine/",
                "Landroid/system/virtualizationservice/",
                "Landroid/os/SystemProperties;",
            )
        }.onFailure {
            Log.w("HyperWin", "hidden API exemption failed", it)
        }
    }
}
