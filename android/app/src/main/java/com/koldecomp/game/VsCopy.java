package com.koldecomp.game;

import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * VS Mode's copies of the game, from the game's process (kol_vs_cpu.h
 * VsPlatform, main_android.cpp): the computer's game, or the other player's
 * on the network, runs unseen in {@link VsCopyService}, a process of its own,
 * because the native runtime runs once per process. And the Wi-Fi's
 * multicast lock, held while a game looks for another on the local network:
 * without it Android drops the broadcasts the two use to find each other.
 */
public final class VsCopy {
    private static final String TAG = "KoLVs";
    private static WifiManager.MulticastLock sLock;

    private VsCopy() {}

    /** Starts a copy with these arguments and switches (NAME=value). */
    static boolean start(String[] argv, String[] env, String log, String dir) {
        Context ctx = SDLActivity.getContext();
        if (ctx == null) return false;
        Intent i = new Intent(ctx, VsCopyService.class);
        i.putExtra(VsCopyService.EXTRA_ARGV, argv);
        i.putExtra(VsCopyService.EXTRA_ENV, env);
        i.putExtra(VsCopyService.EXTRA_LOG, log);
        i.putExtra(VsCopyService.EXTRA_DIR, dir);
        try {
            return ctx.startService(i) != null;
        } catch (RuntimeException e) {
            Log.e(TAG, "the copy's service could not start", e);
            return false;
        }
    }

    /** Holds (true) or lets go of (false) the Wi-Fi's multicast lock. */
    static synchronized void wifiLock(boolean on) {
        Context ctx = SDLActivity.getContext();
        if (ctx == null) return;
        try {
            if (sLock == null) {
                WifiManager wm = (WifiManager) ctx.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wm == null) return;
                sLock = wm.createMulticastLock("kol-vs");
                sLock.setReferenceCounted(false);
            }
            if (on) sLock.acquire();
            else if (sLock.isHeld()) sLock.release();
        } catch (RuntimeException e) {
            Log.w(TAG, "the Wi-Fi's multicast lock", e);
        }
    }
}
