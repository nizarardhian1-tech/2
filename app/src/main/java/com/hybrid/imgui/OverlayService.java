package com.hybrid.imgui;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

public class OverlayService extends Service {
    private static final String CH_ID = "hybrid_tool";

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        createNotificationChannel();
        Notification.Builder nb = new Notification.Builder(this, CH_ID)
            .setContentTitle("IL2Cpp Tool Active")
            .setContentText("Overlay running")
            .setSmallIcon(android.R.drawable.ic_menu_info_details);
        startForeground(1, nb.build());
        return START_STICKY;
    }

    @Override public IBinder onBind(Intent i) { return null; }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                CH_ID, "Hybrid Tool", NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(ch);
        }
    }
}
