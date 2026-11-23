package com.koldecomp.game;

import android.content.Context;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.io.StringWriter;

/**
 * A Java crash's stack trace in the session's log, files/logs/last_run.log
 * (the native side's, kol_crash.h, which the Files app shows), then the
 * system's own handling as before. Installed by the first activity of a run.
 */
final class CrashLog {
    private static boolean sInstalled;

    private CrashLog() {}

    static synchronized void install(Context context) {
        if (sInstalled) return;
        sInstalled = true;
        final File logs = new File(context.getFilesDir(), "logs");
        final Thread.UncaughtExceptionHandler before = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, e) -> {
            try {
                //noinspection ResultOfMethodCallIgnored
                logs.mkdirs();
                StringWriter trace = new StringWriter();
                e.printStackTrace(new PrintWriter(trace));
                try (FileWriter w = new FileWriter(new File(logs, "last_run.log"), true)) {
                    w.write("\n*** crash: Java exception on thread " + thread.getName() + "\n" + trace
                            + "*** end of the crash report\n");
                }
            } catch (Throwable ignored) {
                // (the system's report still follows)
            }
            if (before != null) before.uncaughtException(thread, e);
        });
    }
}
