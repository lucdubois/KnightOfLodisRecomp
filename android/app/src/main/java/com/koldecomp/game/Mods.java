package com.koldecomp.game;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;

import org.libsdl.app.SDL;

import java.io.File;

// A mod: a patch file (IPS, UPS or BPS) made for this game's ROM, applied
// over it at start (gbarecomp --rom-patch; main_android.cpp). It lives in
// files/mod/ under its own name, one at a time.
//
// Options > Mod is the game's own dialog (kol_menu menu_dialog); this class
// only does what it asks (MenuPlatform in kol_menu.h): the system's file
// picker, removing the mod, restarting the app. The picker's answer goes back
// with nativePicked.
public final class Mods {
    private Mods() {}

    static final String DIR = "mod";

    /** The picked mod's name, or null and why not (kol::menu_mod_picked). */
    static native void nativePicked(String name, String error);

    /** A BIOS picked from Options > Technical > BIOS: null, or why not (kol::menu_bios_picked). */
    static native void nativeBiosPicked(String error);

    /** The mod file in files/mod/, or null. */
    static File current(Context ctx) {
        File[] fs = new File(ctx.getFilesDir(), DIR).listFiles();
        if (fs == null) return null;
        for (File f : fs) if (f.isFile()) return f;
        return null;
    }

    static void clear(Context ctx) {
        File[] fs = new File(ctx.getFilesDir(), DIR).listFiles();
        if (fs != null) for (File f : fs) f.delete();
    }

    private static void onUi(java.util.function.Consumer<Activity> run) {
        new Handler(Looper.getMainLooper()).post(() -> {
            Context ctx = SDL.getContext();
            if (ctx instanceof Activity && !((Activity) ctx).isFinishing()) run.accept((Activity) ctx);
        });
    }

    /** From native: the system's file picker (SetupActivity's pick_mod mode). */
    public static void pick() {
        onUi(act -> {
            Intent i = new Intent(act, SetupActivity.class);
            i.putExtra(SetupActivity.EXTRA_PICK_MOD, true);
            act.startActivity(i);
        });
    }

    /** From native (Options > Technical > BIOS, no BIOS file yet): the file picker. */
    public static void pickBios() {
        onUi(act -> {
            Intent i = new Intent(act, SetupActivity.class);
            i.putExtra(SetupActivity.EXTRA_PICK_BIOS, true);
            act.startActivity(i);
        });
    }

    /** From native: files/mod/ emptied (the game plays the original from the next start). */
    public static void remove() {
        Context ctx = SDL.getContext();
        if (ctx != null) clear(ctx);
    }

    /** From native (Options > Licenses): the app's third-party notices,
     *  assets/licenses.txt (tools/make_licenses.py), in a scrolling dialog. */
    public static void showLicenses() {
        onUi(act -> {
            String text;
            try (java.io.InputStream in = act.getAssets().open("licenses.txt")) {
                // (A read loop: InputStream.readAllBytes is API 33, minSdk is 24.)
                java.io.ByteArrayOutputStream all = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) all.write(buf, 0, n);
                text = new String(all.toByteArray(), java.nio.charset.StandardCharsets.UTF_8);
            } catch (Exception e) {
                text = "licenses.txt is missing from this build.";
            }
            android.widget.TextView view = new android.widget.TextView(act);
            view.setText(text);
            view.setTextSize(12);
            view.setTypeface(android.graphics.Typeface.MONOSPACE);
            int pad = (int) (16 * act.getResources().getDisplayMetrics().density);
            view.setPadding(pad, pad, pad, pad);
            view.setTextIsSelectable(true);
            android.widget.ScrollView scroll = new android.widget.ScrollView(act);
            scroll.addView(view);
            new android.app.AlertDialog.Builder(act)
                    .setTitle("Licenses")
                    .setView(scroll)
                    .setPositiveButton("Close", null)
                    .show();
        });
    }

    /** From native: the app ends and starts again. */
    public static void restart() {
        onUi(RestartActivity::restart);
    }
}
