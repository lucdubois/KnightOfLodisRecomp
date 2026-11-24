package com.koldecomp.game;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.GLUtils;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * Options > Display > Bottom Shader (2026-11-24): the second screen's
 * picture through its own RetroArch preset. A GL surface over StatsView on
 * the Thor's lower panel: StatsView draws its page at the canvas's own size
 * (248 wide) into a bitmap and hands it here (show); each frame drawn, that
 * picture is uploaded and native runs the preset from it into the rectangle
 * StatsView would have filled (gbarecomp shader::run_gl, the preset's chain
 * in this surface's context, on this surface's thread).
 *
 * On top of the panel's window (setZOrderOnTop), opaque; touches are not
 * taken here, so they go on to StatsView under it. Drawn when a picture
 * comes (when dirty): StatsView sends one at each of its draws; then a few
 * frames more, so a preset that blends the last frames in (the handheld
 * ones' slow LCD: up to Prev6) settles instead of freezing half-way (the
 * Thor, gameboy.glslp: the list's old rows stayed faint after a scroll).
 */
final class BottomShaderView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private final Object lock = new Object();
    private Bitmap pending;          // the latest picture (a copy), under lock
    private boolean fresh;           // not uploaded yet
    private int settle;              // frames still to draw after the last picture (under lock)
    private static final int SETTLE_FRAMES = 8;   // the history's 7, and one
    private int dx, dy, dw, dh;      // where it goes, the surface's pixels from the top left
    private final Runnable onFail;   // the preset could not draw: StatsView draws again by itself

    // GL thread only.
    private int tex = 0, texW = 0, texH = 0, surfW = 0, surfH = 0;

    BottomShaderView(Context ctx, Runnable onFail) {
        super(ctx);
        this.onFail = onFail;
        setEGLContextClientVersion(2);           // (drivers give a 3.x context, as SDL's)
        setEGLConfigChooser(8, 8, 8, 0, 0, 0);   // no alpha (the Thor's RGBA targets read alpha 0: SDL's GLES2)
        setZOrderOnTop(true);
        setPreserveEGLContextOnPause(true);
        setRenderer(this);
        setRenderMode(RENDERMODE_WHEN_DIRTY);
        setFocusable(false);
        setClickable(false);
    }

    /** UI thread: the page (copied) and where it goes; drawn at the next frame. */
    void show(Bitmap src, int x, int y, int w, int h) {
        synchronized (lock) {
            if (pending == null || pending.getWidth() != src.getWidth() || pending.getHeight() != src.getHeight())
                pending = Bitmap.createBitmap(src.getWidth(), src.getHeight(), Bitmap.Config.ARGB_8888);
            new Canvas(pending).drawBitmap(src, 0f, 0f, null);
            dx = x; dy = y; dw = w; dh = h;
            fresh = true;
            settle = SETTLE_FRAMES;
        }
        requestRender();
    }

    @Override
    public void onSurfaceCreated(GL10 unused, EGLConfig config) {
        // A new context: the last one's preset and texture are gone with it.
        tex = 0;
        texW = texH = 0;
        SecondScreen.bottomGlLost();
        synchronized (lock) { fresh = pending != null; }
    }

    @Override
    public void onSurfaceChanged(GL10 unused, int w, int h) {
        surfW = w;
        surfH = h;
    }

    @Override
    public void onDrawFrame(GL10 unused) {
        int x, y, w, h;
        boolean again;
        synchronized (lock) {
            if (fresh && pending != null) {
                if (tex == 0) {
                    final int[] t = new int[1];
                    GLES20.glGenTextures(1, t, 0);
                    tex = t[0];
                }
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
                GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, pending, 0);   // first row the top
                texW = pending.getWidth();
                texH = pending.getHeight();
                fresh = false;
            }
            x = dx; y = dy; w = dw; h = dh;
            again = settle > 0;
            if (again) --settle;
        }
        GLES20.glViewport(0, 0, surfW, surfH);
        GLES20.glClearColor(0f, 0f, 0f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        if (tex == 0 || w <= 0 || h <= 0) return;
        if (!SecondScreen.shadeBottom(tex, texW, texH, x, y, w, h, surfH)) {
            post(onFail);
            return;
        }
        if (again) requestRender();
    }
}
