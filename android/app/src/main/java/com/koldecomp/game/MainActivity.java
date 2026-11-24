package com.koldecomp.game;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.KeyEvent;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

/**
 * The game. SDLActivity dlopen()s the libraries named below in order and then
 * calls SDL_main in libmain.so, which is defined in android/app/jni/
 * main_android.cpp and hands control to gbarecomp::run_game().
 *
 * BACK opens the enhancement menu: a couple of toggles, plus "Options": the
 * "View size" list, which covers the whole extended view including its
 * height, and the depth of field's strength. All are built from the native
 * side (kol::Setting, the size ladder, the blur choices) rather than
 * duplicated here, so changing them there needs no Java change. "Save states"
 * saves to or loads from a chosen slot; the pad combos only reach slot 1.
 */
public class MainActivity extends SDLActivity {
    private static native int nativeSettingCount();
    private static native String nativeSettingLabel(int index);
    private static native boolean nativeSettingOn(int index);
    private static native void nativeSettingSet(int index, boolean on);
    private static native int nativeWidthCount();
    private static native String nativeWidthLabel(int index);
    private static native int nativeWidthCurrent();
    private static native void nativeWidthSet(int index);
    private static native int nativeBlurCount();
    private static native String nativeBlurLabel(int index);
    private static native int nativeBlurCurrent();
    private static native void nativeBlurSet(int index);
    private static native int nativeSoundCount();
    private static native String nativeSoundLabel(int index);
    private static native int nativeSoundCurrent();
    private static native void nativeSoundSet(int index);
    private static native int nativeVolumeCount();
    private static native String nativeVolumeLabel(int index);
    private static native int nativeVolumeCurrent(int which);
    private static native void nativeVolumeSet(int which, int index);
    private static native void nativeStateSave(int slot);
    private static native void nativeStateLoad(int slot);

    private static final int STATE_SLOTS = 3;

    private AlertDialog mMenu;

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        CrashLog.install(this);   // a Java crash in files/logs/last_run.log
        // The Thor's second panel. No-op on a single-screen device.
        SecondScreen.start(this);
        // Options > Technical > Sync Folder: the saves copied to the player's
        // folder as they change (a thread; nothing when no folder is set).
        SyncFolder.start(this);
        // The game's 60 frames a second, said to the display. Unsaid, a phone
        // with adaptive refresh guesses from what the app presents: a Pixel 6
        // (90 Hz) rendered it at 45 and held the game there (2026-11-17,
        // `renderFrameRate 45.0`, ~44 fps with no thread busy).
        if (mSurface != null && android.os.Build.VERSION.SDK_INT >= 30) {
            mSurface.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
                @Override public void surfaceCreated(android.view.SurfaceHolder h) { wantSixty(h); }
                @Override public void surfaceChanged(android.view.SurfaceHolder h, int f, int w, int hh) { wantSixty(h); }
                @Override public void surfaceDestroyed(android.view.SurfaceHolder h) {}
            });
            wantSixty(mSurface.getHolder());
        }
    }

    private static void wantSixty(android.view.SurfaceHolder h) {
        if (android.os.Build.VERSION.SDK_INT < 30) return;
        final android.view.Surface s = h.getSurface();
        if (s == null || !s.isValid()) return;
        try {
            s.setFrameRate(60f, android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        } catch (Exception e) {
            android.util.Log.w("KoL", "setFrameRate: " + e);
        }
    }

    // Out of sight (Home, another app): the second screen's panel goes too, and
    // comes back with the game (SecondScreen.onBackground / onForeground).
    @Override
    protected void onStop() {
        SecondScreen.onBackground();
        SyncFolder.nudge();   // what changed, copied before the app may be stopped
        super.onStop();
    }

    @Override
    protected void onStart() {
        super.onStart();
        SecondScreen.onForeground();
    }

    @Override
    protected void onDestroy() {
        SecondScreen.stop();
        super.onDestroy();
    }

    /**
     * SDL consumes key events from its own surface, so intercept BACK before
     * the view hierarchy sees it. Everything else falls through to the guest.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            // One screen (no second panel, or Debug > No Second Screen): the
            // game pauses and the options menu comes up over it.
            if (event.getAction() == KeyEvent.ACTION_UP) {
                if (SecondScreen.single()) SecondScreen.toggleOverlayMenu();
                else toggleMenu();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private void toggleMenu() {
        if (mMenu != null && mMenu.isShowing()) {
            mMenu.dismiss();
            mMenu = null;
            return;
        }
        final int count = nativeSettingCount();
        if (count <= 0) return;

        // A setting with no label is retired (kol_settings.cpp): not listed.
        // `index` maps the list's rows back to the settings.
        final java.util.ArrayList<Integer> rows = new java.util.ArrayList<>();
        for (int i = 0; i < count; i++)
            if (nativeSettingLabel(i).length() > 0) rows.add(i);
        final int[] index = new int[rows.size()];
        final CharSequence[] labels = new CharSequence[rows.size()];
        final boolean[] checked = new boolean[rows.size()];
        for (int r = 0; r < rows.size(); r++) {
            index[r] = rows.get(r);
            labels[r] = nativeSettingLabel(index[r]);
            checked[r] = nativeSettingOn(index[r]);
        }

        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("Enhancements");
        b.setMultiChoiceItems(labels, checked,
                new DialogInterface.OnMultiChoiceClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which,
                                        boolean isChecked) {
                        // Applied immediately and persisted natively; there is
                        // no OK button to press, so a mis-tap is undone by
                        // tapping again rather than by cancelling.
                        nativeSettingSet(index[which], isChecked);
                    }
                });
        b.setPositiveButton("Close", null);
        // Choices rather than toggles (the view size, the blur strength) sit
        // behind one button: the dialog has room for three.
        b.setNeutralButton("Options", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                showOptionsMenu();
            }
        });
        b.setNegativeButton("Save states", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                showStateMenu();
            }
        });
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    /** The slot file the runtime writes; its time is shown beside the slot. */
    private String slotStamp(int slot) {
        final File f = new File(getFilesDir(), "totkol.state" + slot);
        if (!f.exists()) return null;
        return new SimpleDateFormat("MMM d, HH:mm", Locale.getDefault())
                .format(new Date(f.lastModified()));
    }

    /**
     * Save to or load from slots 1..STATE_SLOTS. Loading an empty slot is not
     * offered. Slot 1 is also what the pad combos write, stray ones included.
     */
    private void showStateMenu() {
        final CharSequence[] labels = new CharSequence[STATE_SLOTS * 2];
        final int[] slots = new int[STATE_SLOTS * 2];
        final boolean[] save = new boolean[STATE_SLOTS * 2];
        int n = 0;
        for (int s = 1; s <= STATE_SLOTS; s++) {
            final String stamp = slotStamp(s);
            labels[n] = "Save to slot " + s + (stamp != null
                    ? "  (replaces " + stamp + ")" : "  (empty)");
            slots[n] = s; save[n] = true; n++;
        }
        for (int s = 1; s <= STATE_SLOTS; s++) {
            final String stamp = slotStamp(s);
            if (stamp == null) continue;
            labels[n] = "Load slot " + s + "  (" + stamp + ")";
            slots[n] = s; save[n] = false; n++;
        }
        final int count = n;
        final CharSequence[] shown = new CharSequence[count];
        System.arraycopy(labels, 0, shown, 0, count);

        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("Save states");
        b.setItems(shown, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                if (which < 0 || which >= count) return;
                if (save[which]) nativeStateSave(slots[which]);
                else nativeStateLoad(slots[which]);
            }
        });
        b.setNegativeButton("Cancel", null);
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    /** The list of choices, each opening its own single-choice list. */
    private void showOptionsMenu() {
        final CharSequence[] labels = new CharSequence[] {
            "View size…",
            "Depth of field strength…  (" + nativeBlurLabel(nativeBlurCurrent()) + ")",
            "Sound…  (" + nativeSoundLabel(nativeSoundCurrent()) + ")",
            "Music volume…  (" + nativeVolumeLabel(nativeVolumeCurrent(0)) + ")",
            "Effects volume…  (" + nativeVolumeLabel(nativeVolumeCurrent(1)) + ")",
        };
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("Options");
        b.setItems(labels, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                if (which == 0) showWidthMenu();
                else if (which == 1) showBlurMenu();
                else if (which == 2) showSoundMenu();
                else showVolumeMenu(which - 3);
            }
        });
        b.setNegativeButton("Cancel", null);
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    /** How strongly the depth of field blurs the farthest terrain. */
    private void showBlurMenu() {
        final int count = nativeBlurCount();
        if (count <= 0) return;
        final CharSequence[] labels = new CharSequence[count];
        for (int i = 0; i < count; i++) labels[i] = nativeBlurLabel(i);
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("Depth of field strength");
        b.setSingleChoiceItems(labels, nativeBlurCurrent(),
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        nativeBlurSet(which);
                        dialog.dismiss();
                    }
                });
        b.setNegativeButton("Cancel", null);
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    /** The hardware mix, or the music re-rendered in high quality. */
    private void showSoundMenu() {
        final int count = nativeSoundCount();
        if (count <= 0) return;
        final CharSequence[] labels = new CharSequence[count];
        for (int i = 0; i < count; i++) labels[i] = nativeSoundLabel(i);
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("Sound");
        b.setSingleChoiceItems(labels, nativeSoundCurrent(),
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        nativeSoundSet(which);
                        dialog.dismiss();
                    }
                });
        b.setNegativeButton("Cancel", null);
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    /** Music (0) or effects (1) volume. Needs a high-quality sound mode. */
    private void showVolumeMenu(final int which) {
        final int count = nativeVolumeCount();
        if (count <= 0) return;
        final CharSequence[] labels = new CharSequence[count];
        for (int i = 0; i < count; i++) labels[i] = nativeVolumeLabel(i);
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle(which == 0 ? "Music volume (high quality sound)"
                              : "Effects volume (high quality sound)");
        b.setSingleChoiceItems(labels, nativeVolumeCurrent(which),
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int w) {
                        nativeVolumeSet(which, w);
                        dialog.dismiss();
                    }
                });
        b.setNegativeButton("Cancel", null);
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    private void showWidthMenu() {
        final int count = nativeWidthCount();
        if (count <= 0) return;
        final CharSequence[] labels = new CharSequence[count];
        for (int i = 0; i < count; i++) labels[i] = nativeWidthLabel(i);

        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("View size");
        b.setSingleChoiceItems(labels, nativeWidthCurrent(),
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        // Applied live and persisted natively; 240x160 is
                        // simply the entry that turns the extended view off.
                        nativeWidthSet(which);
                        dialog.dismiss();
                    }
                });
        b.setNegativeButton("Cancel", null);
        b.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) { mMenu = null; }
        });
        mMenu = b.create();
        mMenu.show();
    }

    @Override
    protected String[] getLibraries() {
        // SDL2 first: libmain.so links against it.
        return new String[] { "SDL2", "main" };
    }
}
