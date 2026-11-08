package com.fallsena.fallsena

import android.os.Build
import android.os.Bundle
import io.flutter.embedding.android.FlutterActivity

class MainActivity : FlutterActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Force-opened grace "Are you OK?" flow (#P3-USERCHECK-APP-2): the
        // full-screen-intent notification relaunches this activity, so it must
        // be allowed to appear over the lock screen and wake the display.
        //
        // TRADE-OFF: flutter_local_notifications' full-screen intent simply
        // relaunches the single main activity with no custom extra to
        // distinguish an alarm launch from a normal launch. Because this app is
        // single-activity, these flags therefore apply to EVERY launch — the app
        // will show over the lockscreen even on an ordinary open. Accepted as an
        // MVP trade-off.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        }
    }
}
