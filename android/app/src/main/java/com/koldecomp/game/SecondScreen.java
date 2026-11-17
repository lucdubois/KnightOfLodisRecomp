package com.koldecomp.game;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;


/**
 * Drives the AYN Thor's second panel.
 *
 * The device exposes it as an ordinary Android presentation display — from
 * `dumpsys display`:
 *
 *   Display 4: "Screen-2", 1240 x 1080, FLAG_PRESENTATION, state ON
 *
 * so no vendor SDK is involved. Native code hands us ARGB frames through
 * push(); we blit them into an ImageView inside a Presentation on that display.
 *
 * Nothing here reads or writes guest state. The panel is a present-time
 * shadow: display only, never an input to emulation.
 *
 * With one screen (no presentation display, or Debug > No Second Screen in a
 * dev build) there is no panel: BACK then holds the game still and shows the
 * same page, with the options menu open, over the game on the main screen
 * (the overlay below). It goes away, and the game goes on, when the menu
 * closes (BACK again, B, or its button).
 */
public final class SecondScreen {
    private static final String TAG = "KoLScreen2";

    private static Activity sActivity;
    private static Presentation sPresentation;
    private static StatsView sView;
    // One screen: the same view over the game, shown while the menu is open.
    private static FrameLayout sOverlay;
    private static StatsView sOverlayView;
    private static volatile boolean sDebugSingle = false;

    private SecondScreen() {}

    /** No second panel to draw on: none found, or switched off for testing. */
    static boolean single() { return sPresentation == null || sDebugSingle; }

    /** Where the native pictures go. */
    private static StatsView target() { return single() ? sOverlayView : sView; }

    static native void nativePause(boolean on);

    private static void buildOverlay(Activity activity) {
        try {
            sOverlayView = new StatsView(activity);
            sOverlayView.setFocusable(false);        // key focus stays on the game's surface
            sOverlayView.onMenuClosed = SecondScreen::overlayClosed;
            sOverlay = new FrameLayout(activity);
            sOverlay.setBackgroundColor(0xFF000000); // the whole screen: nothing of the game shows
            sOverlay.setFocusable(false);
            sOverlay.setClickable(true);             // a touch beside the page goes nowhere
            // The page takes the whole screen and scales itself by whole
            // pixels to its height, as on the second screen: the menu at the
            // top, the thin bar under it. (A box sized from DisplayMetrics came
            // out a little short of the panel, which dropped the scale a step
            // and left a tall bar.)
            sOverlay.addView(sOverlayView, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
            sOverlay.setVisibility(View.GONE);
            activity.addContentView(sOverlay, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        } catch (Exception e) {
            Log.e(TAG, "could not build the one-screen overlay", e);
            sOverlay = null;
            sOverlayView = null;
        }
    }

    /** The menu opened or closed through its own toggle until it is as wanted. */
    private static int[] setMenuOpen(boolean want) {
        int[] img = null;
        for (int i = 0; i < 2; i++) {
            img = menuToggle();
            if ((img != null) == want) break;
        }
        return img;
    }

    private static void showOverlay() {
        if (sOverlay == null) return;
        // Only as wide as the page (StatsView draws it at a whole scale k to
        // the height, 248 k wide, centred): the strips beside it stay the
        // game's surface, so the on-screen controls show there and drive the
        // menu (asked 2026-11-17 for the on-screen controls to stay while the
        // back menu is open); the game is held, its input still read.
        if (sActivity != null) {
            final View root = sActivity.getWindow().getDecorView();
            final int w = root.getWidth(), h = root.getHeight();
            final int k = (int) Math.floor(Math.min(w / 248f, h / 184f));
            if (k >= 1 && 248 * k < w) {
                sOverlay.setLayoutParams(new FrameLayout.LayoutParams(248 * k, ViewGroup.LayoutParams.MATCH_PARENT,
                                                                      Gravity.CENTER_HORIZONTAL));
            }
        }
        sOverlay.setVisibility(View.VISIBLE);
        nativePause(true);
    }

    private static void overlayClosed() {
        if (sOverlay != null) sOverlay.setVisibility(View.GONE);
        nativePause(false);
    }

    /** BACK with one screen (UI thread): the game held and the menu over it, or back to the game. */
    static void toggleOverlayMenu() {
        if (sOverlay == null || sOverlayView == null) return;
        if (sOverlay.getVisibility() == View.VISIBLE) {
            sOverlayView.setMenuImageFromNative(setMenuOpen(false));
            overlayClosed();
            return;
        }
        final int[] img = setMenuOpen(true);
        if (img == null) { setMenuOpen(false); return; }   // no cartridge art yet: nothing to show
        sOverlayView.setMenuImageFromNative(img);
        showOverlay();
    }

    /** Debug > No Second Screen changed (from native). */
    public static void setSingle(final boolean on) {
        final Activity a = sActivity;
        if (a == null) { sDebugSingle = on; return; }
        a.runOnUiThread(() -> {
            if (on == sDebugSingle) return;
            // The menu was open on the side that goes away: close it there.
            final StatsView from = target();
            if (from != null && from.menuShown()) from.setMenuImageFromNative(setMenuOpen(false));
            if (!on) overlayClosed();
            sDebugSingle = on;
            try {
                if (sPresentation != null) {
                    if (on) { if (sPresentation.isShowing()) sPresentation.hide(); }
                    else if (!sPresentation.isShowing()) sPresentation.show();
                }
            } catch (Exception e) {
                Log.e(TAG, "could not toggle presentation", e);
            }
        });
    }

    /** Called from MainActivity once the SDL activity exists. */
    public static void start(Activity activity) {
        sActivity = activity;
        activity.runOnUiThread(() -> buildOverlay(activity));
        DisplayManager dm =
                (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
        if (dm == null) {
            Log.w(TAG, "no DisplayManager");
            return;
        }
        Display[] displays = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (displays == null || displays.length == 0) {
            Log.w(TAG, "no presentation display; second screen disabled");
            return;
        }
        final Display target = displays[0];
        Log.i(TAG, "presentation display: id=" + target.getDisplayId()
                + " name=" + target.getName());

        activity.runOnUiThread(() -> {
            try {
                sPresentation = new Presentation(sActivity, target);
                // Touchable but never focusable: a tap on the second screen
                // must not move key focus off the game's display, or the pad
                // would stop reaching the game.
                if (sPresentation.getWindow() != null)
                    sPresentation.getWindow().addFlags(
                            android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
                sView = new StatsView(sPresentation.getContext());
                sPresentation.setContentView(sView, new ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));
                sPresentation.show();
                Log.i(TAG, "presentation shown");
            } catch (Exception e) {
                Log.e(TAG, "could not show presentation", e);
            }
        });
    }

    /**
     * Whether a presentation display was found and the panel created on it.
     * Called from native, which leaves the game's own unit box out while the
     * second screen shows the unit.
     */
    public static boolean available() {
        return sPresentation != null && !sDebugSingle;
    }

    /**
     * Player toggle from the settings menu, called from native. Hides the
     * presentation rather than leaving the last hovered unit frozen on screen.
     */
    public static void setEnabled(final boolean on) {
        if (sActivity == null || sPresentation == null) return;
        sActivity.runOnUiThread(() -> {
            try {
                if (on) {
                    if (!sDebugSingle && !sPresentation.isShowing()) sPresentation.show();
                } else if (sPresentation.isShowing()) {
                    sPresentation.hide();
                }
            } catch (Exception e) {
                Log.e(TAG, "could not toggle presentation", e);
            }
        });
    }

    /** The debug instant win has started the outcome check and waits for its answer. */
    public static void setAutoWin(final boolean on) {
        final StatsView v = target();
        if (v == null) return;
        v.setAutoWin(on);
    }

    /** A HELP card over the page (Organize's item or spellbook stock: the one
     *  under the cursor), 248 wide, length / 248 tall; null to take it away. */
    public static void pushPageHelp(final int[] argb) {
        final StatsView v = target();
        if (v == null) return;
        v.post(() -> v.setPageHelp(argb));
    }

    /** The bar's Save and Load changed (on, greyed, none): draw it again. */
    public static void barChanged() {
        final StatsView v = target();
        if (v != null) v.postInvalidate();
    }

    /** The unit's status sheet, rendered from the cartridge's own art. */
    public static void pushSheetImage(final int[] argb, final int w,
                                      final int h) {
        final StatsView v = target();
        if (v == null) return;
        v.setSheetImage(argb, w, h);
    }

    // The options menu (native kol_menu): each returns the menu's new image,
    // 240x184 ARGB, or null once it is closed. menuButton() is the corner
    // button, 24x24.
    /** The menu's image after the pad changed it, or null once closed. */
    public static void pushMenuImage(final int[] argb) {
        final StatsView v = target();
        if (v == null) return;
        if (single() && sActivity != null) {
            // One screen: a menu opened from the pad (L3+R3's Save States)
            // brings the overlay up; its closing puts it away (onMenuClosed).
            sActivity.runOnUiThread(() -> {
                v.setMenuImageFromNative(argb);
                if (argb != null && sOverlay != null && sOverlay.getVisibility() != View.VISIBLE)
                    showOverlay();
            });
            return;
        }
        v.setMenuImageFromNative(argb);
    }

    static native int[] menuToggle();
    static native int[] menuTouch(int action, float x, float y);
    // The Save States page's grid (kol::menu_states_*): StatsView scrolls its rows.
    /** Rows, version, ask count, asked place, top, bottom, row height; null off the page. */
    static native int[] menuStatesInfo();
    static native int[] menuStatesRow(int row, int h);
    static native void menuStatesScroll(float scroll);
    static native int[] menuButton();
    /** The zone under the page, 248 x h, the buttons inside it (`pressed` 0
     *  Option, 1 Save, 2 Load pushed in, -1 none; Save and Load only with
     *  `world` 1, greyed with 2); null if h is too small. */
    static native int[] menuZone(int h, int pressed, int world);
    /** The bar's world buttons: low bits 0 none, 1 on (the world map), 2
     *  greyed (one of the menu's screens is up); with 1, +4 Training and +8
     *  Shop greyed (not in the menu where the army is). */
    static native int menuWorld();
    /** The game's save / load screen, as its system menu opens them. */
    static native void worldSave();
    static native void worldLoad();
    /** The bar's button's help card (its game name and help text), 248 wide,
     *  as tall as the card (length / 248); null for none. */
    static native int[] barHelp(int button);
    /** Item (0), Spell (1), Class (2) (Organize's R menu), Training (3), Shop (4). */
    static native void worldItem(int item);
    /** The game's pointing hand, 16 x 8; null before the ROM art. */
    static native int[] menuHand();
    static native int[] menuArrow(boolean down);

    // The sheet's lower card (native kol_sheet): flip between EQUIP./TALENT
    // and the EMBLEM page; a tap in canvas pixels selects an emblem there.
    static native void sheetFlipPage();
    static native void sheetPeek(boolean on);

    /** The sheet with its other lower page, while a finger is on it. */
    public static void pushSheetPeek(final int[] argb) {
        final StatsView v = target();
        if (v == null) return;
        v.setPeekImage(argb);
    }
    static native boolean sheetTap(float x, float y);

    // The battle's units while the player browses the map (native
    // kol_unit_list): the list's picture, 248 wide and `h` tall (null and 0
    // once there is no list), and the record of the unit on the cursor's
    // cell, 0 on empty ground. A tap in its canvas pixels selects that unit
    // on the battlefield.
    public static void pushUnitList(final int[] argb, final int h, final int onUnit) {
        final StatsView v = target();
        if (v == null) return;
        v.setUnitList(argb, h, onUnit, unitListPin(), unitListFaces(), unitListShop() != 0);
    }
    static native int unitListTap(float x, float y);   // the unit's record, 0 none
    static native int unitListPress(float x, float y, int phase);   // the magic shop's buttons
    static native int unitListShop();   // the list composed last was a shop's
    static native int unitListPin();   // the picture's top rows that do not scroll
    static native int unitListFaces();   // the faces the magic shop's picture last had

    /** Debug > Tile Info's editor page (native kol_edit_panel), 248x184 ARGB,
     *  or null once Tile Info is off. It takes the sheet's place. */
    public static void pushEditPanel(final int[] argb) {
        final StatsView v = target();
        if (v == null) return;
        v.setEditPanel(argb);
    }
    /** A touch on the editor page, canvas pixels; 0 down, 1 move, 2 up, 3 cancel. */
    static native void editTouch(int action, float x, float y);

    /** The opening's mountain (scene 1) or the title's clouds (2) below the
     *  top screen, w x h (null and 0 once neither scene is up). */
    public static void pushSceneStrip(final int[] argb, final int w, final int h, final int scene) {
        final StatsView v = target();
        if (v == null) return;
        v.setSceneStrip(argb, w, h, scene);
    }

    public static void stop() {
        final Activity a = sActivity;
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (sPresentation != null) {
                sPresentation.dismiss();
                sPresentation = null;
            }
        });
    }
}
