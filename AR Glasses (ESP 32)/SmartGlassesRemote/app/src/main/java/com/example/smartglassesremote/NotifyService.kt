package com.example.smartglassesremote

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.text.TextUtils

class NotifyService : NotificationListenerService() {

    override fun onListenerConnected() {
        // Optional: tell ESP32 we’re ready
        if (BtClient.isConnected()) {
            BtClient.sendLine("#TEXT|Notifications enabled")
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val extras = sbn.notification.extras
        val title = extras.getCharSequence("android.title")?.toString()?.trim().orEmpty()
        val text  = extras.getCharSequence("android.text")?.toString()?.trim().orEmpty()
        if (title.isEmpty() && text.isEmpty()) return

        val msg = if (text.isNotEmpty()) "$title: $text" else title
        // Keep it short for the 128x32 display
        val clipped = if (msg.length > 60) msg.substring(0, 60) + "…" else msg

        if (BtClient.isConnected()) {
            BtClient.sendLine("#TEXT|$clipped")
        }
    }
}

