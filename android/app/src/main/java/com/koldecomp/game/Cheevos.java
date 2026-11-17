package com.koldecomp.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.content.Context;
import android.graphics.Bitmap;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.libsdl.app.SDL;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayDeque;

// RetroAchievements (Options > Achievements; src/kol_cheevos.h), the parts
// that need Android: the HTTP requests rcheevos asks for, the login form, and
// a toast for each unlock. Called from native code (main_android.cpp).
//
// Only the server's login token is kept (by native code, files/
// retroachievements.cfg); the password typed here goes to the server once
// and is not stored.
public final class Cheevos {
    private static final String TAG = "KoL";

    private Cheevos() {}

    // The password login, answered on a native worker (kol::cheevos_login).
    private static native void nativeLogin(String user, String password);

    /** One request, blocking (a native worker thread calls it). `post` null:
     *  GET. Returns the status as 4 big-endian bytes, then the body; null when
     *  no answer came at all. */
    public static byte[] post(String url, String post, String contentType, String userAgent) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setRequestProperty("User-Agent", userAgent);
            if (post != null) {
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                if (contentType != null) c.setRequestProperty("Content-Type", contentType);
                byte[] out = post.getBytes("UTF-8");
                c.setFixedLengthStreamingMode(out.length);
                try (OutputStream os = c.getOutputStream()) { os.write(out); }
            }
            int status = c.getResponseCode();
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            body.write((status >>> 24) & 0xFF);
            body.write((status >>> 16) & 0xFF);
            body.write((status >>> 8) & 0xFF);
            body.write(status & 0xFF);
            if (in != null) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) body.write(buf, 0, n);
                in.close();
            }
            return body.toByteArray();
        } catch (Exception e) {
            Log.w(TAG, "cheevos: http " + e);
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** A line for the player (an unlock, a login): a toast on the game's screen. */
    public static void notify(final String text) {
        new Handler(Looper.getMainLooper()).post(() -> {
            Context ctx = SDL.getContext();
            if (ctx != null) Toast.makeText(ctx, text, Toast.LENGTH_LONG).show();
        });
    }

    // ── The unlock popup ────────────────────────────────────────────────
    // Drawn by native code in the game's window style (menu_cheevo_popup_render);
    // shown in the top-right corner of the game's screen, nearest-neighbour at
    // 3x per 1080 lines: it slides in from the right, holds, slides out. Unlocks
    // that come together queue up.
    private static final long HOLD_MS = 4000, SLIDE_MS = 250;
    private static final ArrayDeque<Bitmap> sQueue = new ArrayDeque<>();
    private static boolean sShowing;

    /** From native (the guest thread): one popup, ARGB w x h. */
    public static void showPopup(int[] argb, int w, int h) {
        final Bitmap b;
        try {
            b = Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888);
        } catch (IllegalArgumentException e) {
            return;
        }
        new Handler(Looper.getMainLooper()).post(() -> {
            sQueue.add(b);
            if (!sShowing) next();
        });
    }

    private static void next() {
        final Bitmap b = sQueue.poll();
        Context ctx = SDL.getContext();
        if (b == null || !(ctx instanceof Activity) || ((Activity) ctx).isFinishing()) {
            sShowing = false;
            sQueue.clear();
            return;
        }
        sShowing = true;
        final Activity act = (Activity) ctx;
        int screenH = act.getWindow().getDecorView().getHeight();
        if (screenH <= 0) screenH = act.getResources().getDisplayMetrics().heightPixels;
        final int scale = Math.max(1, Math.round(screenH * 3f / 1080f));
        final Bitmap big = Bitmap.createScaledBitmap(b, b.getWidth() * scale, b.getHeight() * scale, false);
        final ImageView view = new ImageView(act);
        view.setImageBitmap(big);
        final int margin = 4 * scale;
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(big.getWidth(), big.getHeight(),
                Gravity.TOP | Gravity.END);
        lp.topMargin = margin;
        lp.rightMargin = margin;
        act.addContentView(view, lp);
        view.setTranslationX(big.getWidth() + margin);
        view.animate().translationX(0).setDuration(SLIDE_MS).setListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator a) {
                view.animate().translationX(big.getWidth() + margin).setStartDelay(HOLD_MS)
                        .setDuration(SLIDE_MS).setListener(new AnimatorListenerAdapter() {
                            @Override public void onAnimationEnd(Animator a2) {
                                ViewGroup parent = (ViewGroup) view.getParent();
                                if (parent != null) parent.removeView(view);
                                next();
                            }
                        });
            }
        });
    }

    /** The login form: a name and a password, sent to native on Log In. */
    public static void showLogin() {
        new Handler(Looper.getMainLooper()).post(() -> {
            Context ctx = SDL.getContext();
            if (!(ctx instanceof Activity) || ((Activity) ctx).isFinishing()) return;
            LinearLayout box = new LinearLayout(ctx);
            box.setOrientation(LinearLayout.VERTICAL);
            int pad = (int) (20 * ctx.getResources().getDisplayMetrics().density);
            box.setPadding(pad, pad / 2, pad, 0);
            TextView note = new TextView(ctx);
            note.setText("Your RetroAchievements account. The password is sent to "
                    + "retroachievements.org once and not stored; only the login token is kept. "
                    + "Achievements are earned in softcore.");
            box.addView(note);
            final EditText user = new EditText(ctx);
            user.setHint("User name");
            user.setSingleLine(true);
            user.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            box.addView(user);
            final EditText pass = new EditText(ctx);
            pass.setHint("Password");
            pass.setSingleLine(true);
            pass.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            box.addView(pass);
            new AlertDialog.Builder(ctx)
                    .setTitle("RetroAchievements")
                    .setView(box)
                    .setPositiveButton("Log In", (d, w) -> {
                        String u = user.getText().toString().trim();
                        String p = pass.getText().toString();
                        if (!u.isEmpty() && !p.isEmpty()) nativeLogin(u, p);
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
        });
    }
}
