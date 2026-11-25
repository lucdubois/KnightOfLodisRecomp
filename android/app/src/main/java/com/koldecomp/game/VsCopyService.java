package com.koldecomp.game;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.Process;
import android.util.Log;

/**
 * Runs VS Mode's unseen copy of the game (the computer's, or the other
 * player's on the network) in this process (":vscopy" in the manifest): the
 * native runtime runs once per process, and the game's own runs in the main
 * one. Started by {@link VsCopy#start}; the copy links to the game by a local
 * socket and ends by itself when its link or its channel closes, and this
 * process with it. One copy at a time: a start while one runs ends this
 * process (the session that asked times out and the player tries again).
 */
public class VsCopyService extends Service {
    static final String EXTRA_ARGV = "argv";
    static final String EXTRA_ENV = "env";
    static final String EXTRA_LOG = "log";
    static final String EXTRA_DIR = "dir";
    private static final String TAG = "KoLVsCopy";
    private static boolean sRunning;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        final String[] argv = intent.getStringArrayExtra(EXTRA_ARGV);
        final String[] env = intent.getStringArrayExtra(EXTRA_ENV);
        final String log = intent.getStringExtra(EXTRA_LOG);
        final String dir = intent.getStringExtra(EXTRA_DIR);
        synchronized (VsCopyService.class) {
            if (sRunning) {
                Log.w(TAG, "a copy is running already: this process ends");
                Process.killProcess(Process.myPid());
                return START_NOT_STICKY;
            }
            sRunning = true;
        }
        Thread t = new Thread(() -> {
            try {
                System.loadLibrary("SDL2");
                // The game being played (this is another process: the choice
                // is the one SetupActivity kept, Game.java).
                final int g = Game.current(this);
                System.loadLibrary(Game.library(g >= 0 ? g : Game.USA));
                int code = nativeRun(argv, env, log, dir);
                Log.i(TAG, "the copy ended (" + code + ")");
            } catch (Throwable e) {
                Log.e(TAG, "the copy failed", e);
            }
            Process.killProcess(Process.myPid());
        }, "vscopy");
        t.start();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private static native int nativeRun(String[] argv, String[] env, String log, String dir);
}
