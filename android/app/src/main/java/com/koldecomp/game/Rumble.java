package com.koldecomp.game;

import android.content.Context;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

import org.libsdl.app.SDL;

// Rumble (Options > Rumble; src/kol_rumble.h): the device's own vibrator.
// The Thor's built-in controller has no motors of its own (Android lists no
// vibrator on its input device); the device's vibrator has amplitude control,
// so an effect's waveform plays as it is, step by step. Called from native
// code (main_android.cpp) on whatever thread the effect starts on.
public final class Rumble {
    private static final String TAG = "KoL";
    private static Vibrator sVibrator;
    private static boolean sLogged;

    private Rumble() {}

    private static synchronized Vibrator vibrator() {
        if (sVibrator != null) return sVibrator;
        Context ctx = SDL.getContext();
        if (ctx == null) return null;
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager vm = (VibratorManager) ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            if (vm != null) sVibrator = vm.getDefaultVibrator();
        } else {
            sVibrator = (Vibrator) ctx.getSystemService(Context.VIBRATOR_SERVICE);
        }
        if (sVibrator != null && !sLogged) {
            sLogged = true;
            Log.i(TAG, "rumble: vibrator " + (sVibrator.hasVibrator() ? "present" : "absent")
                    + ", amplitude control " + (Build.VERSION.SDK_INT >= 26 && sVibrator.hasAmplitudeControl()));
        }
        return sVibrator;
    }

    // `ms` the steps' durations, `levels` their strengths 0..255 (0 a pause).
    public static void play(int[] ms, int[] levels) {
        Vibrator v = vibrator();
        if (v == null || !v.hasVibrator() || ms == null || levels == null || ms.length == 0) return;
        try {
            if (Build.VERSION.SDK_INT >= 26) {
                long[] timings = new long[ms.length];
                int[] amps = new int[ms.length];
                long total = 0;
                int peak = 0;
                for (int i = 0; i < ms.length; ++i) {
                    timings[i] = Math.max(1, ms[i]);
                    amps[i] = Math.max(0, Math.min(255, levels[i]));
                    total += timings[i];
                    peak = Math.max(peak, amps[i]);
                }
                if (v.hasAmplitudeControl()) {
                    v.vibrate(VibrationEffect.createWaveform(timings, amps, -1));
                } else if (peak > 0) {
                    // No amplitude control: each step on or off, so a pause in it
                    // (the 40 ms every rumble starts late with, kol_rumble) stays one.
                    int[] onoff = new int[amps.length];
                    for (int i = 0; i < amps.length; ++i) onoff[i] = amps[i] > 0 ? VibrationEffect.DEFAULT_AMPLITUDE : 0;
                    v.vibrate(VibrationEffect.createWaveform(timings, onoff, -1));
                }
            } else {
                long total = 0;
                for (int t : ms) total += Math.max(1, t);
                v.vibrate(total);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "rumble: " + e);
        }
    }
}
