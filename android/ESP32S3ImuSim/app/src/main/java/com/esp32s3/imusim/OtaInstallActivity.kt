package com.esp32s3.imusim

import android.app.Activity
import android.content.Intent
import android.os.Build
import android.os.Bundle

/** PackageInstaller confirmation trampoline. */
class OtaInstallActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val extra = if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(Intent.EXTRA_INTENT)
        }
        if (extra != null) {
            startActivity(extra)
        }
        finish()
    }
}
