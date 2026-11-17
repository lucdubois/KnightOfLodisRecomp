package com.koldecomp.game;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.View;

/**
 * The second screen. Every page is drawn natively from the cartridge's own
 * art and arrives here as a picture: the unit's status sheet (kol_sheet), the
 * battle's unit list (kol_unit_list) and the options menu (kol_menu). This
 * view places them at the top of the screen at a whole-number scale, animates
 * the swipes and the card over the list, and passes touches back. It never
 * drives emulation.
 */
public final class StatsView extends View {
    private final Paint blit = new Paint();   // no filtering: keep it pixel-art
    private volatile Bitmap sheetImage = null;   // the sheet, rendered from ROM
    private volatile boolean autoWin = false;    // the debug win is waiting
    private final Paint autoWinText = new Paint(Paint.ANTI_ALIAS_FLAG);

    public StatsView(Context c) {
        super(c);
        setBackgroundColor(Color.BLACK);
    }

    /** The debug instant win has started the outcome check and waits for its answer. */
    public void setAutoWin(boolean on) {
        autoWin = on;
        postInvalidate();
    }

    /** "auto-win" in the top-left corner, over the band above the sheet,
     *  while the debug win waits for the next unit to fall. */
    private void drawAutoWin(Canvas c) {
        if (!autoWin) return;
        autoWinText.setTextSize(34f);
        autoWinText.setColor(Color.rgb(255, 196, 64));
        autoWinText.setFakeBoldText(true);
        c.drawText("auto-win", 20f, 48f, autoWinText);
    }

    /** The unit's status sheet, rendered natively from the cartridge's own
     *  art (kol_sheet.cpp): frame, labels, digits, icons, item pictures and
     *  the game's font. */
    public void setSheetImage(int[] argb, int w, int h) {
        if (argb == null) {
            // No unit any more (native: off the battle map, no status
            // window): the list, or black, in its place.
            sheetImage = null;
            awaitNew = false;
            holdImg = null;
            postInvalidate();
            return;
        }
        if (w <= 0 || h <= 0 || argb.length < w * h) return;
        try {
            sheetImage = Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888);
        } catch (IllegalArgumentException e) {
            return;
        }
        if (awaitNew) { awaitNew = false; holdImg = null; }
        // On the UI thread, after any list update sent before it (native
        // sends the list first), so the wait it ends is this unit's.
        post(() -> { cardAwaitSheet = false; invalidate(); });
        postInvalidate();
    }

    /** From the second-screen thread: the sheet with its other lower page,
     *  while a finger is on the lower card (null: nothing to swipe to). */
    public void setPeekImage(int[] argb) {
        Bitmap b = null;
        if (argb != null && argb.length >= MENU_W * MENU_H) {
            try { b = Bitmap.createBitmap(argb, MENU_W, MENU_H, Bitmap.Config.ARGB_8888); }
            catch (IllegalArgumentException e) { b = null; }
        }
        peekImg = b;
        postInvalidate();
    }

    // The lower card's pages (EQUIP./TALENT and EMBLEM) under the finger:
    // a horizontal drag that starts on the lower card moves it with the
    // finger, the other page following it in beside; on release it snaps,
    // to the other page past a third of the width or on a flick that way,
    // back otherwise. A tap there goes to the sheet (an emblem's text).
    // Canvas pixels throughout.
    private static final int SHEET_LOWER_Y = 88;    // kSheetLowerRow * 8
    private float sheetLeft = 0f, sheetTop = 0f, sheetK = 1f;
    private float downX, downY, dragOff = 0f;
    private boolean sheetTouch = false, dragging = false;
    private android.view.VelocityTracker velocity = null;
    private Bitmap fromImg = null;                 // the page as the drag began
    private volatile Bitmap peekImg = null;        // the other page
    private Bitmap toImg = null;                   // it, frozen at the release
    private volatile Bitmap holdImg = null;        // shown until the flipped sheet arrives
    private volatile boolean awaitNew = false;
    private long snapStart = -1, snapMs = 0;
    private float snapFrom = 0f, snapTo = 0f;

    private void sheetTouchEvent(android.view.MotionEvent e, int act, float x, float y) {
        final float cx = (x - sheetLeft) / sheetK, cy = (y - sheetTop) / sheetK;
        if (act == android.view.MotionEvent.ACTION_DOWN) {
            sheetTouch = sheetImage != null;
            if (!sheetTouch) return;
            if (snapStart >= 0) finishSnap();
            downX = cx; downY = cy;
            dragging = false;
            dragOff = 0f;
            if (downY >= SHEET_LOWER_Y) {
                fromImg = sheetImage;
                peekImg = null;
                SecondScreen.sheetPeek(true);
            }
            if (downY >= SHEET_LOWER_Y || listImage != null) {
                velocity = android.view.VelocityTracker.obtain();
                velocity.addMovement(e);
            }
            return;
        }
        if (!sheetTouch) return;
        if (velocity != null) velocity.addMovement(e);
        final float dx = cx - downX, dy = cy - downY;
        final float iw = MENU_W;
        if (act == android.view.MotionEvent.ACTION_MOVE) {
            // Down, while the list is under the card: the card follows the
            // finger toward the bottom (the list shows as it goes).
            if (!dragging && !cardDragging && listImage != null && dy > 8f && dy > Math.abs(dx)) {
                cardDragging = true;
                releasePeek();
            }
            if (cardDragging) {
                cardDrag = Math.max(0f, dy) / cardTravel();
                invalidate();
                return;
            }
            if (!dragging && fromImg != null && peekImg != null && Math.abs(dx) > 6f && Math.abs(dx) > Math.abs(dy))
                dragging = true;
            if (dragging) {
                dragOff = Math.max(-iw, Math.min(iw, dx));
                invalidate();
            }
            return;
        }
        sheetTouch = false;
        if (cardDragging) {
            cardDragging = false;
            float vy = 0f;
            if (velocity != null && act == android.view.MotionEvent.ACTION_UP) {
                velocity.computeCurrentVelocity(1000);
                vy = velocity.getYVelocity() / sheetK;   // canvas px per second
            }
            final boolean away = act == android.view.MotionEvent.ACTION_UP &&
                    (cardDrag > 0.25f || vy > 300f);
            if (away) dismissed = listOn;
            cardAnimate(cardDrag, away ? 1f : 0f);
            cardDrag = -1f;
            if (velocity != null) { velocity.recycle(); velocity = null; }
            return;
        }
        if (dragging) {
            float vx = 0f;
            if (velocity != null && act == android.view.MotionEvent.ACTION_UP) {
                velocity.computeCurrentVelocity(1000);
                vx = velocity.getXVelocity() / sheetK;   // canvas px per second
            }
            final boolean flick = Math.abs(vx) > 300f && Math.signum(vx) == Math.signum(dragOff);
            // The other page as it is now: once the page flips, the second
            // screen's thread would send the one being left as "the other".
            toImg = peekImg;
            SecondScreen.sheetPeek(false);
            final boolean commit = act == android.view.MotionEvent.ACTION_UP && toImg != null &&
                    (Math.abs(dragOff) > iw / 3f || flick);
            if (commit) {
                SecondScreen.sheetFlipPage();
                awaitNew = true;
            }
            startSnap(commit ? Math.signum(dragOff) * iw : 0f);
            dragging = false;
        } else {
            releasePeek();
            if (act == android.view.MotionEvent.ACTION_UP && Math.abs(dx) < 8f && Math.abs(dy) < 8f)
                SecondScreen.sheetTap(cx, cy);
        }
        if (velocity != null) { velocity.recycle(); velocity = null; }
    }

    private void startSnap(float to) {
        snapFrom = dragOff;
        snapTo = to;
        snapMs = 70 + (long) (160f * Math.abs(to - dragOff) / MENU_W);
        snapStart = android.os.SystemClock.uptimeMillis();
        invalidate();
    }

    private void finishSnap() {
        if (snapTo != 0f && awaitNew) holdImg = toImg;   // the flip is on its way
        snapStart = -1;
        dragOff = 0f;
        releasePeek();
    }

    private void releasePeek() {
        SecondScreen.sheetPeek(false);
        fromImg = null;
        peekImg = null;
        toImg = null;
    }

    // ── The unit list (native kol_unit_list) ────────────────────────────
    // While the player browses the battle map the second screen shows the
    // battle's units. When the cursor is on a unit, that unit's card lies
    // over the list, sliding up from the bottom as it arrives; dragging it
    // down (past a quarter, or a flick) puts it away until the cursor moves
    // to another unit. The list scrolls under a finger and a tap on a unit
    // selects it on the battlefield. Canvas pixels are the list's own.
    private Bitmap listImage = null;             // UI thread only
    private int listOn = 0, dismissed = 0;       // unit records; 0 none
    private float listScroll = 0f, listLeft = 0f, listTop = 0f, listK = 1f;
    // The magic shop's picture: `listPin` rows that stay, then whole rows of
    // faces. At rest the list sits on a row, so two whole rows always show.
    private int listPin = 0;
    private static final int LIST_ROW = 48, ARROW = 8;
    private int transPin = 88;   // the box's rows, kept for the way back from the faces
    private final Paint blackPaint = new Paint();
    { blackPaint.setColor(Color.BLACK); }
    private boolean listSnap = false, listButton = false;
    private boolean listShop = false;   // in a shop: no bottom bar, the list takes its room
    private float listSnapTo = 0f;
    private long listSnapLast = 0;
    private Bitmap arrowUp = null, arrowDown = null;
    // Between the army's cards and the magic shop's faces (README "Spells on
    // the shop's list"): transT 0 is the cards, 1 the faces.
    private static final float TRANS_SECONDS = 0.2f, TRANS_STAGGER = 0.3f;
    private static final int CARD_W = 124, CARD_H = 64, FACE = 48, FACE_INSET = 8, FACES_PER_ROW = 5;
    private boolean transOn = false;
    private float transT = 0f, transDir = 1f, cardsScroll = 0f, facesScroll = 0f;
    private long transLast = 0;
    private int transN = 0;
    private Bitmap transCards = null, transFaces = null;
    private final Paint transPaint = new Paint();
    private boolean listTouch = false, listScrolling = false;
    private float listDownX, listDownY, listDownScroll;
    // A flick carries the list on, slowing (LIST_FLING_DECAY a second) until
    // it stops or reaches an end (asked 2026-10-29 for smoother
    // scrolling on the bottom screen). Canvas px a second; 0 none.
    private android.view.VelocityTracker listVelocity = null;
    private float listFlingV = 0f;
    private long listFlingLast = 0;
    private static final float LIST_FLING_DECAY = 3.5f, LIST_FLING_MIN = 15f;
    private float cardPos = 0f;                  // 0 in place .. 1 below the screen
    private float cardDrag = -1f;                // while a finger drags it; -1 not
    private boolean cardDragging = false;
    private long cardAnimStart = -1, cardAnimMs = 0;
    private float cardAnimFrom = 0f, cardAnimTo = 0f;
    private boolean cardAwaitSheet = false;      // the new unit's card is on its way
    private long cardAwaitSince = 0;
    private static final long CARD_WAIT_MS = 150;

    /** From the second-screen thread: the list's picture (null: no list),
     *  its height, the unit on the cursor's cell, and the top rows that stay
     *  when it scrolls (the spell's description in the shop). */
    public void setUnitList(final int[] argb, final int h, final int onUnit, final int pin, final int faces,
                            final boolean shop) {
        post(() -> {
            listShop = shop && argb != null;
            if (pin > 0) transPin = pin;
            final boolean picture = argb != null && h > 0 && argb.length >= MENU_W * h;
            if (faces > 0) transN = faces;
            if (pin != listPin) {   // the other layout: the faces fly between them
                if (listImage != null && picture && transN > 0) {
                    if (pin > 0) {
                        if (!transOn) { transCards = listImage; cardsScroll = listScroll; transT = 0f; facesScroll = 0f; }
                        transDir = 1f;
                    } else {
                        if (!transOn) { transFaces = listImage; facesScroll = listScroll; transT = 1f; }
                        else if (transDir > 0f) transFaces = listImage;
                        transDir = -1f;
                    }
                    transOn = true;
                    transLast = android.os.SystemClock.uptimeMillis();
                    listImage = null;   // the old picture stays as it was
                }
                listPin = pin;
                listScroll = pin > 0 ? 0f : cardsScroll;
                listFlingV = 0f;
                listSnap = false;
            }
            if (!picture) {
                listImage = null;
                listOn = 0;
                dismissed = 0;
                listScroll = 0f;
                transOn = false;
                transCards = transFaces = null;
                cardsScroll = 0f;
            } else {
                Bitmap b = listImage;
                if (b == null || b.getHeight() != h) {
                    b = Bitmap.createBitmap(MENU_W, h, Bitmap.Config.ARGB_8888);
                    listImage = b;
                }
                b.setPixels(argb, 0, MENU_W, 0, 0, MENU_W, h);
                if (transOn) {
                    if (pin > 0) transFaces = b;
                    else transCards = b;
                }
                if (onUnit != listOn) {
                    // From one unit straight to another with the card up, the
                    // card stays and takes the new sheet; from the list, it
                    // waits for that sheet, then slides up.
                    final boolean up = listOn != 0 && listOn != dismissed && cardPos <= 0f &&
                            cardAnimStart < 0 && !cardDragging;
                    if (onUnit != dismissed) dismissed = 0;
                    cardAwaitSheet = onUnit != 0 && !up;
                    cardAwaitSince = android.os.SystemClock.uptimeMillis();
                    listOn = onUnit;
                }
            }
            invalidate();
        });
    }

    /** The card should be up: no list, or the cursor on a unit not put away. */
    private boolean cardWanted() {
        if (listImage == null) return true;
        if (listOn == 0 || listOn == dismissed) return false;
        return !cardAwaitSheet ||
               android.os.SystemClock.uptimeMillis() - cardAwaitSince > CARD_WAIT_MS;
    }

    /** Only the list is showing: no sheet at all (the world map's army list,
     *  or the battle's list with no unit picked yet), or the card away and
     *  not moving. With no sheet the card's position is never advanced (it
     *  moves only while the sheet is drawn), so it cannot be the test: after
     *  loading a save onto the world map the list took no finger until a
     *  window had shown a sheet once. */
    private boolean listOnly() {
        if (listImage == null) return false;
        if (sheetImage == null) return true;
        return !cardWanted() && cardPos >= 1f && cardAnimStart < 0 && !cardDragging;
    }

    private float cardTravel() {
        return Math.max(1f, (getHeight() - sheetTop) / sheetK);   // canvas px
    }

    private void cardAnimate(float from, float to) {
        cardAnimFrom = from;
        cardAnimTo = to;
        cardPos = from;
        cardAnimMs = 60 + (long) (160f * Math.abs(to - from));
        cardAnimStart = android.os.SystemClock.uptimeMillis();
        invalidate();
    }

    /** The card's position now, moving it toward where it should be. */
    private float cardPosition() {
        if (listImage == null) { cardAnimStart = -1; cardPos = 0f; return 0f; }
        if (cardDragging) return cardPos = Math.min(1f, Math.max(0f, cardDrag));
        final float want = cardWanted() ? 0f : 1f;
        if (cardAnimStart < 0 && cardPos != want) cardAnimate(cardPos, want);
        else if (cardAnimStart >= 0 && cardAnimTo != want) cardAnimate(cardPos, want);
        if (cardAnimStart >= 0) {
            final float t = Math.min(1f, (android.os.SystemClock.uptimeMillis() - cardAnimStart) / (float) cardAnimMs);
            final float ease = 1f - (1f - t) * (1f - t) * (1f - t);
            cardPos = cardAnimFrom + (cardAnimTo - cardAnimFrom) * ease;
            if (t >= 1f) { cardAnimStart = -1; cardPos = cardAnimTo; }
            else postInvalidateOnAnimation();
        }
        if (cardAwaitSheet && listOn != 0 && listOn != dismissed) postInvalidateOnAnimation();
        return cardPos;
    }

    /** The list, at the sheet's scale and top, scrolled. Clears the view. */
    private void drawList(Canvas c) {
        c.drawColor(Color.BLACK);
        final Bitmap img = listImage;
        if (img == null) return;
        final float w = getWidth(), h = getHeight();
        int k = (int) Math.floor(Math.min(w / MENU_W, h / MENU_H));
        if (k < 1) k = 1;
        listK = k;
        listLeft = (w - MENU_W * k) * 0.5f;
        listTop = 0f;   // at the top, as the sheet
        if (transOn) {
            final long now = android.os.SystemClock.uptimeMillis();
            final float dt = Math.min(0.05f, (now - transLast) / 1000f);
            transLast = now;
            transT = Math.max(0f, Math.min(1f, transT + transDir * dt / TRANS_SECONDS));
            if ((transDir > 0f && transT >= 1f) || (transDir < 0f && transT <= 0f)) {
                transOn = false;
                transCards = transFaces = null;
            } else if (transCards != null && transFaces != null) {
                drawListTransition(c, k);
                postInvalidateOnAnimation();
                return;
            }
        }
        // The sheet's window, 184 lines: the black below it is the same on
        // every page; a longer list scrolls inside it.
        // In a shop the bar's room is the list's. (The magic shop's faces keep to
        // whole rows counted from the 184 lines: two always show, the rest peeks.)
        final int windowH = listWindowH(k);
        final float maxScroll = Math.max(0f, img.getHeight() - (listPin > 0 ? listPin + 2 * LIST_ROW : windowH));
        if (listFlingV != 0f) {
            final long now = android.os.SystemClock.uptimeMillis();
            final float dt = Math.min(0.05f, (now - listFlingLast) / 1000f);
            listFlingLast = now;
            listScroll += listFlingV * dt;
            listFlingV *= (float) Math.exp(-LIST_FLING_DECAY * dt);
            if (Math.abs(listFlingV) < LIST_FLING_MIN || listScroll <= 0f || listScroll >= maxScroll) listFlingV = 0f;
            else postInvalidateOnAnimation();
        }
        if (listSnap) {
            final long now = android.os.SystemClock.uptimeMillis();
            final float dt = Math.min(0.05f, (now - listSnapLast) / 1000f);
            listSnapLast = now;
            final float to = Math.max(0f, Math.min(maxScroll, listSnapTo));
            listScroll += (to - listScroll) * (1f - (float) Math.exp(-18f * dt));
            if (Math.abs(to - listScroll) < 0.5f) { listScroll = to; listSnap = false; }
            else postInvalidateOnAnimation();
        }
        listScroll = Math.max(0f, Math.min(maxScroll, listScroll));
        final int top = (int) Math.floor(listScroll);
        final int pin = Math.max(0, Math.min(listPin, Math.min(MENU_H, img.getHeight())));
        final int rows = Math.min(img.getHeight() - pin - top, windowH - pin + 1);
        final float y0 = listTop + pin * k - (listScroll - top) * k;
        c.save();
        c.clipRect(listLeft, listTop + pin * k, listLeft + MENU_W * k, listTop + windowH * k);
        if (rows > 0)
            c.drawBitmap(img, new Rect(0, pin + top, MENU_W, pin + top + rows),
                         new RectF(listLeft, y0, listLeft + MENU_W * k, y0 + rows * k), blit);
        c.restore();
        if (pin > 0)
            c.drawBitmap(img, new Rect(0, 0, MENU_W, pin),
                         new RectF(listLeft, listTop, listLeft + MENU_W * k, listTop + pin * k), blit);
        if (pin > 0 && maxScroll > 0f) {
            // The menu's arrows, in the 8 px right of the faces: more that way.
            loadArrows();
            if (arrowUp != null) {
                final float ax = listLeft + (MENU_W - ARROW) * k;
                if (listScroll > 0.5f)
                    c.drawBitmap(arrowUp, null, new RectF(ax, listTop + pin * k, ax + ARROW * k,
                                                          listTop + (pin + ARROW) * k), blit);
                if (listScroll < maxScroll - 0.5f)
                    c.drawBitmap(arrowDown, null, new RectF(ax, listTop + (windowH - ARROW) * k, ax + ARROW * k,
                                                            listTop + windowH * k), blit);
            }
        }
    }

    /** The list, then the card over it at its position. */
    private void drawSheetArea(Canvas c, Bitmap img) {
        final float pos = cardPosition();
        if (listImage != null && pos > 0f) drawList(c);
        else c.drawColor(Color.BLACK);
        if (pos >= 1f) return;
        final float off = pos * (getHeight() - sheetTop);
        c.save();
        c.translate(0f, off);
        drawSheetImage(c, img);
        c.restore();
    }

    private int listWindowH(int k) {
        return listShop ? Math.max(MENU_H, getHeight() / k) : MENU_H;
    }

    private static float smooth(float t) {
        t = Math.max(0f, Math.min(1f, t));
        return t * t * (3f - 2f * t);
    }

    /** The cards fade, the description comes down from the top, and each
     *  portrait flies between its card and its place among the faces, a few at
     *  a time, taking on the faces' dimming. The way back is this reversed. */
    private void drawListTransition(Canvas c, int k) {
        final Bitmap cards = transCards, faces = transFaces;
        final float e = smooth(transT);
        final int windowH = listWindowH(k);
        final float right = listLeft + MENU_W * k, bottom = listTop + windowH * k;
        final int slot = Math.min(windowH, listPin > 0 ? listPin : transPin);
        c.save();
        c.clipRect(listLeft, listTop, right, bottom);
        // The cards, each with the hole its portrait left.
        final float fade = Math.max(0f, 1f - e * 1.5f);
        if (fade > 0f) {
            final int top = (int) Math.floor(cardsScroll);
            final int rows = Math.min(cards.getHeight() - top, windowH + 1);
            final float y0 = listTop - (cardsScroll - top) * k;
            transPaint.setAlpha(Math.round(fade * 255f));
            if (rows > 0)
                c.drawBitmap(cards, new Rect(0, top, MENU_W, top + rows),
                             new RectF(listLeft, y0, right, y0 + rows * k), transPaint);
            for (int i = 0; i < transN; ++i) {
                final float hx = listLeft + (i % 2 * CARD_W + FACE_INSET) * k;
                final float hy = listTop + (i / 2 * CARD_H + FACE_INSET - cardsScroll) * k;
                c.drawRect(hx, hy, hx + FACE * k, hy + FACE * k, blackPaint);
            }
        }
        // The description, down from above the window.
        final float dy = listTop - slot * (1f - e) * k;
        c.drawBitmap(faces, new Rect(0, 0, MENU_W, Math.min(slot, faces.getHeight())),
                     new RectF(listLeft, dy, right, dy + Math.min(slot, faces.getHeight()) * k), blit);
        // The portraits, over all that, in the description's way or not.
        final int last = Math.max(1, transN - 1);
        for (int i = 0; i < transN; ++i) {
            final float s = smooth(transT * (1f + TRANS_STAGGER) - TRANS_STAGGER * i / last);
            final int cx = i % 2 * CARD_W + FACE_INSET, cy = i / 2 * CARD_H + FACE_INSET;
            final int fx = i % FACES_PER_ROW * FACE, fy = slot + i / FACES_PER_ROW * FACE;
            final float x = cx + (fx - cx) * s;
            final float y = (cy - cardsScroll) + (fy - facesScroll - (cy - cardsScroll)) * s;
            final float px = listLeft + x * k, py = listTop + y * k;
            if (py >= bottom || py + FACE * k <= listTop) continue;
            final RectF to = new RectF(px, py, px + FACE * k, py + FACE * k);
            if (s < 1f && cy + FACE <= cards.getHeight())
                c.drawBitmap(cards, new Rect(cx, cy, cx + FACE, cy + FACE), to, blit);
            if (s > 0f && fy + FACE <= faces.getHeight()) {
                transPaint.setAlpha(Math.round(s * 255f));
                c.drawBitmap(faces, new Rect(fx, fy, fx + FACE, fy + FACE), to, transPaint);
            }
        }
        c.restore();
    }

    private void snapListTo(float to) {
        listSnapTo = to;
        listSnap = true;
        listSnapLast = android.os.SystemClock.uptimeMillis();
        postInvalidateOnAnimation();
    }

    private void listTouchEvent(android.view.MotionEvent e, int act, float x, float y) {
        // The magic shop's slots and Yes / No work as the bottom bar's buttons:
        // pushed in under the finger, acting if it lets go there. Native, as
        // they are part of the picture; the pinned rows do not scroll.
        if (listPin > 0) {
            final float px = (x - listLeft) / listK, py = (y - listTop) / listK;
            if (act == android.view.MotionEvent.ACTION_DOWN) {
                if (py < listPin && SecondScreen.unitListPress(px, py, 0) != 0) {
                    listButton = true;
                    listTouch = true;
                    return;
                }
            } else if (listButton) {
                final boolean up = act == android.view.MotionEvent.ACTION_UP;
                final boolean cancel = act == android.view.MotionEvent.ACTION_CANCEL;
                SecondScreen.unitListPress(px, py, up ? 2 : cancel ? 3 : 1);
                if (up || cancel) {
                    listButton = false;
                    listTouch = false;
                }
                return;
            }
        }
        if (act == android.view.MotionEvent.ACTION_DOWN) {
            final boolean flinging = listFlingV != 0f;
            listFlingV = 0f;                 // a finger catches a gliding list
            listSnap = false;
            listTouch = true;
            listScrolling = flinging;        // ... and holds it, not a tap
            listDownX = x; listDownY = y;
            listDownScroll = listScroll;
            if (listVelocity != null) listVelocity.recycle();
            listVelocity = android.view.VelocityTracker.obtain();
            listVelocity.addMovement(e);
            return;
        }
        if (!listTouch) return;
        if (listVelocity != null) listVelocity.addMovement(e);
        final float dx = (x - listDownX) / listK, dy = (y - listDownY) / listK;
        if (act == android.view.MotionEvent.ACTION_MOVE) {
            if (!listScrolling && Math.abs(dy) > 6f) {
                // From here, without the 6 px it took to tell a scroll from
                // a tap (the list jumped them at once).
                listScrolling = true;
                listDownY = y;
                listDownScroll = listScroll;
                return;
            }
            if (listScrolling) {
                listScroll = listDownScroll - dy;   // clamped as it is drawn
                invalidate();
            }
            return;
        }
        listTouch = false;
        if (act == android.view.MotionEvent.ACTION_UP && listScrolling && listVelocity != null) {
            listVelocity.computeCurrentVelocity(1000);
            final float v = -listVelocity.getYVelocity() / listK;   // canvas px a second
            if (listPin > 0) {   // onto a row, a flick carrying it on
                snapListTo(Math.round((listScroll + v * 0.15f) / LIST_ROW) * (float) LIST_ROW);
            } else if (Math.abs(v) > 60f) {
                listFlingV = v;
                listFlingLast = android.os.SystemClock.uptimeMillis();
                postInvalidateOnAnimation();
            }
        }
        if (listVelocity != null) { listVelocity.recycle(); listVelocity = null; }
        if (act == android.view.MotionEvent.ACTION_UP && !listScrolling &&
            Math.abs(dx) < 8f && Math.abs(dy) < 8f) {
            // A tap by the arrows: a row that way, as in the menu.
            final float tapY = (y - listTop) / listK;
            final boolean onArrows = listPin > 0 && tapY >= listPin && (x - listLeft) / listK >= MENU_W - ARROW &&
                    listImage != null && listImage.getHeight() > listPin + 2 * LIST_ROW;
            if (onArrows)
                snapListTo(Math.round(listScroll / LIST_ROW) * (float) LIST_ROW +
                           (tapY < listPin + LIST_ROW ? -LIST_ROW : LIST_ROW));
            // The cursor goes to the unit; its card follows once the cursor
            // is there (setUnitList). The card put away comes back if it is
            // that unit's, the cursor already being on it.
            // The pinned rows do not scroll.
            final int rec = onArrows ? 0 : SecondScreen.unitListTap((x - listLeft) / listK,
                    tapY < listPin ? tapY : tapY + listScroll);
            if (rec != 0 && rec == dismissed && rec == listOn) {
                dismissed = 0;
                cardAwaitSheet = false;
                invalidate();
            }
        }
        listScrolling = false;
    }

    /** The sheet at the largest whole-number scale that fits, at the top
     *  of the screen and centred across,
     *  unfiltered, so every game pixel stays a crisp square. (Next and
     *  previous unit are the top and left face buttons; there are no on-screen ones.) */
    private void drawSheetImage(Canvas c, Bitmap img) {
        final float w = getWidth(), h = getHeight();
        int k = (int) Math.floor(Math.min(w / img.getWidth(), h / img.getHeight()));
        if (k < 1) k = 1;
        final float dw = img.getWidth() * k, dh = img.getHeight() * k;
        final float x = (w - dw) * 0.5f, y = 0f;   // at the top of the screen
        sheetLeft = x; sheetTop = y; sheetK = k;
        final RectF full = new RectF(x, y, x + dw, y + dh);
        float off = dragOff;
        if (snapStart >= 0) {
            final float t = Math.min(1f, (android.os.SystemClock.uptimeMillis() - snapStart) / (float) snapMs);
            final float ease = 1f - (1f - t) * (1f - t) * (1f - t);
            off = snapFrom + (snapTo - snapFrom) * ease;
            if (t >= 1f) { finishSnap(); off = 0f; }
        }
        final Bitmap from = fromImg, other = snapStart >= 0 ? toImg : peekImg, hold = holdImg;
        final int iw = img.getWidth(), ih = img.getHeight();
        final float ly = y + SHEET_LOWER_Y * k;
        final Rect lower = new Rect(0, SHEET_LOWER_Y, iw, ih);
        if ((dragging || snapStart >= 0) && from != null && off != 0f) {
            // The top card as it is now; the two pages side by side below.
            c.drawBitmap(img, new Rect(0, 0, iw, SHEET_LOWER_Y), new RectF(x, y, x + dw, ly), blit);
            final float o = Math.round(off) * k;
            c.save();
            c.clipRect(x, ly, x + dw, y + dh);
            c.drawBitmap(from, lower, new RectF(x + o, ly, x + o + dw, y + dh), blit);
            if (other != null) {
                final float in = o - Math.signum(off) * dw;
                c.drawBitmap(other, lower, new RectF(x + in, ly, x + in + dw, y + dh), blit);
            }
            c.restore();
            if (snapStart >= 0) postInvalidateOnAnimation();
            return;
        }
        c.drawBitmap(img, null, full, blit);
        if (hold != null && awaitNew)
            c.drawBitmap(hold, lower, new RectF(x, ly, x + dw, y + dh), blit);
    }

    // ── The options menu (native kol_menu) ──────────────────────────────
    // A button in the bottom-right corner (the game's System icon on a
    // parchment tile) opens it over the panel, in the sheet's place and at
    // its scale. While it is open every touch goes to it, in canvas pixels.
    // The second screen's canvas, kol::kSheetW x kSheetH: 248 wide, so the
    // Thor's 1240 px take it at 5x edge to edge (as the opening's strip).
    private static final int MENU_W = 248, MENU_H = 184, BUTTON = 24, BUTTON_SCALE = 3;
    private volatile Bitmap menuImage = null;
    private Bitmap buttonImage = null;
    // The band under the page, by height, world map (Save and Load shown) and
    // the button pushed in (-1 none): a handful of pictures, made once each.
    private final java.util.HashMap<Integer, Bitmap> zoneImages = new java.util.HashMap<>();
    // The bar's buttons, as kol::kZone...: Option, Save, Load at the right;
    // Item, Spell, Class (Organize's R menu), Training, Shop at the left (the
    // world map's).
    private static final int ZONE_OPTION = 0, ZONE_SAVE = 1, ZONE_LOAD = 2,
                             ZONE_ITEM = 3, ZONE_SPELL = 4, ZONE_CLASS = 5,
                             ZONE_TRAINING = 6, ZONE_SHOP = 7, ZONE_BUTTONS = 8;
    private static final int ZONE_GAP = 4;   // kol::kZoneGap
    // The game's pointing hand, beside the button while the menu is open
    // (still; the one in the menu bobs).
    private static final int HAND_W = 16, HAND_H = 8, HAND_GAP = 2;
    private Bitmap handImage = null;
    private final RectF menuRect = new RectF();
    private final RectF buttonRect = new RectF();   // Option
    // The bar's whole band: a touch that starts there is the bar's, even off
    // its buttons or on a greyed one, never the list's or the sheet's under it.
    private final RectF zoneRect = new RectF();
    private boolean zoneSwallow = false;
    // Every button's rect, empty when it takes no touch (hidden or greyed);
    // and every drawn one's, greyed too (a long press shows their help).
    private final RectF[] zoneRects = new RectF[ZONE_BUTTONS];
    private final RectF[] zoneVisRects = new RectF[ZONE_BUTTONS];
    {
        for (int i = 0; i < ZONE_BUTTONS; ++i) { zoneRects[i] = new RectF(); zoneVisRects[i] = new RectF(); }
    }

    // ── The bar's help: a long press on a button shows the game's name for it
    // and its help text as a card over the page (kol::menu_zone_help_render).
    // While it is up: the same button closes it, another switches to its
    // help, anywhere else closes it; none of those taps acts.
    private static final long LONG_PRESS_MS = 450;
    private int helpButton = -1;          // the card shown, -1 none
    private Bitmap helpImage = null;
    private int helpPending = -1;         // a press that becomes the card if held
    private final Runnable longPress = () -> {
        if (helpPending < 0) return;
        showHelp(helpPending);
        helpPending = -1;
        buttonDown = false;   // the press is the card's now: no action on release
        zoneSwallow = true;
    };

    private void showHelp(int b) {
        Bitmap img = null;
        try {
            final int[] a = SecondScreen.barHelp(b);
            if (a != null && a.length >= MENU_W)
                img = Bitmap.createBitmap(a, MENU_W, a.length / MENU_W, Bitmap.Config.ARGB_8888);
        } catch (Throwable t) { /* native not ready */ }
        helpImage = img;
        helpButton = img != null ? b : -1;
        invalidate();
    }

    // The game's own HELP for what its screen's cursor is on (the stock
    // lists), from the second-screen thread; under a long-pressed one.
    private Bitmap pageHelpImage = null;

    /** UI thread: the page's HELP card, or null. */
    public void setPageHelp(int[] argb) {
        Bitmap img = null;
        if (argb != null && argb.length >= MENU_W) {
            try { img = Bitmap.createBitmap(argb, MENU_W, argb.length / MENU_W, Bitmap.Config.ARGB_8888); }
            catch (IllegalArgumentException e) { img = null; }
        }
        pageHelpImage = img;
        invalidate();
    }

    private void closeHelp() {
        helpButton = -1;
        helpImage = null;
        invalidate();
    }

    /** The card over the page (a long-pressed button's, else the page's
     *  own), the page under it dimmed to 9/16 as the sheet's own HELP cards
     *  dim it. */
    private void drawBarHelp(Canvas c) {
        final Bitmap img = helpImage != null ? helpImage : pageHelpImage;
        if (img == null || menuRect.isEmpty()) return;
        final android.graphics.Paint dim = new android.graphics.Paint();
        dim.setColor(Color.argb(112, 0, 0, 0));
        c.drawRect(menuRect, dim);
        final float k = menuScale;
        c.drawBitmap(img, null, new RectF(menuRect.left, menuRect.top, menuRect.left + MENU_W * k,
                                          menuRect.top + img.getHeight() * k), blit);
    }

    /** kol::menu_zone_button_live: whether button b takes a touch. */
    private static boolean zoneLive(int b, int world) {
        if (b == ZONE_OPTION) return true;
        if ((world & 16) != 0 && b >= ZONE_ITEM && b <= ZONE_CLASS) return true;   // in Organize
        if ((world & 3) != 1) return false;
        if (b == ZONE_TRAINING && (world & 4) != 0) return false;
        return !(b == ZONE_SHOP && (world & 8) != 0);
    }
    /** kol::menu_zone_button_x, canvas pixels. */
    private static int zoneButtonX(int b) {
        if (b >= ZONE_ITEM) return 8 + (b - ZONE_ITEM) * (BUTTON + ZONE_GAP);
        return MENU_W - 8 - BUTTON - (b == ZONE_OPTION ? 0 : (3 - b) * (BUTTON + ZONE_GAP));
    }
    private float menuScale = 1f;
    private boolean buttonDown = false;
    private int buttonId = ZONE_OPTION;          // the button the finger went down on
    private boolean buttonInside = false;        // the finger still on it: pushed in

    // ── The Save States page's grid ─────────────────────────────────────
    // Scrolled here as the army's cards are (a picture a row, a finger, a
    // glide): the page's frame and title come from native, as do taps and the
    // pad; asked 2026-11-05 for scrolling as fluid as the world map's character
    // cards. Canvas pixels; statesRows -1 off the page.
    private int statesRows = -1, statesVersion = -1, statesSeq = -1;
    private int statesTop = 0, statesBottom = 0, statesRowH = 1;
    private Bitmap statesSeen = null;
    private final java.util.HashMap<Integer, Bitmap> statesCache = new java.util.HashMap<>();
    private float statesScroll = 0f, statesPushed = -1f, statesSnapTo = 0f, statesFlingV = 0f;
    private boolean statesSnap = false, statesTouch = false, statesScrolling = false;
    private long statesLast = 0;
    private float statesDownX, statesDownY, statesDownScroll;
    private android.view.VelocityTracker statesVelocity = null;

    private void loadArrows() {
        if (arrowUp != null) return;
        final int[] u = SecondScreen.menuArrow(false), d = SecondScreen.menuArrow(true);
        if (u != null && d != null) {
            arrowUp = Bitmap.createBitmap(u, ARROW, ARROW, Bitmap.Config.ARGB_8888);
            arrowDown = Bitmap.createBitmap(d, ARROW, ARROW, Bitmap.Config.ARGB_8888);
        }
    }

    /** A new picture of the page may mean new rows, or the pad moved the grid. */
    private void readStatesInfo(Bitmap m) {
        if (m == statesSeen) return;
        statesSeen = m;
        final int[] i = m == null ? null : SecondScreen.menuStatesInfo();
        final boolean was = statesRows >= 0;
        if (i == null) {
            statesRows = -1;
            statesCache.clear();
            statesFlingV = 0f;
            statesSnap = false;
            return;
        }
        statesRows = i[0];
        statesTop = i[4];
        statesBottom = i[5];
        statesRowH = i[6];
        if (i[1] != statesVersion) {
            statesVersion = i[1];
            statesCache.clear();
        }
        if (i[2] != statesSeq) {
            statesSeq = i[2];
            statesFlingV = 0f;
            if (was) {
                statesSnapTo = i[3];
                statesSnap = true;
                statesLast = android.os.SystemClock.uptimeMillis();
            } else {
                statesScroll = i[3];
                statesSnap = false;
            }
        }
    }

    private void drawStatesGrid(Canvas c, Bitmap m, float k) {
        readStatesInfo(m);
        if (statesRows <= 0) return;
        final int band = statesBottom - statesTop;
        final float max = Math.max(0f, statesRows * statesRowH - band);
        if (statesFlingV != 0f) {
            final long now = android.os.SystemClock.uptimeMillis();
            final float dt = Math.min(0.05f, (now - statesLast) / 1000f);
            statesLast = now;
            statesScroll += statesFlingV * dt;
            statesFlingV *= (float) Math.exp(-LIST_FLING_DECAY * dt);
            if (Math.abs(statesFlingV) < LIST_FLING_MIN || statesScroll <= 0f || statesScroll >= max) statesFlingV = 0f;
            else postInvalidateOnAnimation();
        }
        if (statesSnap) {
            final long now = android.os.SystemClock.uptimeMillis();
            final float dt = Math.min(0.05f, (now - statesLast) / 1000f);
            statesLast = now;
            final float to = Math.max(0f, Math.min(max, statesSnapTo));
            statesScroll += (to - statesScroll) * (1f - (float) Math.exp(-18f * dt));
            if (Math.abs(to - statesScroll) < 0.5f) { statesScroll = to; statesSnap = false; }
            else postInvalidateOnAnimation();
        }
        statesScroll = Math.max(0f, Math.min(max, statesScroll));
        if (statesScroll != statesPushed) {
            statesPushed = statesScroll;
            SecondScreen.menuStatesScroll(statesScroll);
        }
        final float left = menuRect.left, top = menuRect.top + statesTop * k;
        c.save();
        c.clipRect(left, top, left + MENU_W * k, menuRect.top + statesBottom * k);
        final int first = (int) (statesScroll / statesRowH);
        final int last = Math.min(statesRows - 1, (int) ((statesScroll + band) / statesRowH));
        // Whole device pixels, so every row of the grid lands alike.
        final float y0 = top + Math.round((first * statesRowH - statesScroll) * k);
        for (int r = first; r <= last; ++r) {
            Bitmap b = statesCache.get(r);
            if (b == null) {
                final int[] px = SecondScreen.menuStatesRow(r, statesRowH);
                if (px == null) continue;
                b = Bitmap.createBitmap(px, MENU_W, statesRowH, Bitmap.Config.ARGB_8888);
                statesCache.put(r, b);
            }
            final float y = y0 + (r - first) * statesRowH * k;
            c.drawBitmap(b, null, new RectF(left, y, left + MENU_W * k, y + statesRowH * k), blit);
        }
        c.restore();
        if (statesCache.size() > 24) {   // those far from the view go first
            final java.util.Iterator<Integer> it = statesCache.keySet().iterator();
            while (it.hasNext()) {
                final int r = it.next();
                if (r < first - 6 || r > last + 6) it.remove();
            }
        }
        loadArrows();
        if (arrowUp != null) {
            final float ax = left + (MENU_W - 16) * k;
            if (statesScroll > 0.5f)
                c.drawBitmap(arrowUp, null, new RectF(ax, top, ax + ARROW * k, top + ARROW * k), blit);
            if (statesScroll < max - 0.5f) {
                final float ay = menuRect.top + (statesBottom - ARROW) * k;
                c.drawBitmap(arrowDown, null, new RectF(ax, ay, ax + ARROW * k, ay + ARROW * k), blit);
            }
        }
    }

    /** A drag or a flick is the grid's; a touch that stays where it went down
     *  is native's: a tap loads, a hold marks for deleting, the title goes back. */
    private void statesTouchEvent(android.view.MotionEvent e, int act, float x, float y) {
        final float cx = (x - menuRect.left) / menuScale, cy = (y - menuRect.top) / menuScale;
        if (act == android.view.MotionEvent.ACTION_DOWN) {
            final boolean moving = statesFlingV != 0f;
            statesFlingV = 0f;                // a finger catches a gliding grid
            statesSnap = false;
            statesTouch = true;
            statesScrolling = moving;         // ... and holds it, not a tap
            statesDownX = x; statesDownY = y;
            statesDownScroll = statesScroll;
            if (statesVelocity != null) statesVelocity.recycle();
            statesVelocity = android.view.VelocityTracker.obtain();
            statesVelocity.addMovement(e);
            if (!moving) SecondScreen.menuTouch(0, cx, cy);
            return;
        }
        if (statesVelocity != null) statesVelocity.addMovement(e);
        if (act == android.view.MotionEvent.ACTION_MOVE) {
            final float dy = (y - statesDownY) / menuScale;
            if (!statesScrolling && Math.abs(dy) > 6f) {
                // From here, without the 6 px it took to tell a scroll from a tap.
                statesScrolling = true;
                statesDownY = y;
                statesDownScroll = statesScroll;
                SecondScreen.menuTouch(3, cx, cy);
                return;
            }
            if (statesScrolling) {
                statesScroll = statesDownScroll - dy;   // clamped as it is drawn
                invalidate();
            }
            return;
        }
        if (act != android.view.MotionEvent.ACTION_UP && act != android.view.MotionEvent.ACTION_CANCEL) return;
        final boolean up = act == android.view.MotionEvent.ACTION_UP;
        if (statesScrolling) {
            if (up && statesVelocity != null) {
                statesVelocity.computeCurrentVelocity(1000);
                final float v = -statesVelocity.getYVelocity() / menuScale;   // canvas px a second
                if (Math.abs(v) > 60f) {
                    statesFlingV = v;
                    statesLast = android.os.SystemClock.uptimeMillis();
                    postInvalidateOnAnimation();
                }
            }
        } else {
            setMenuImage(SecondScreen.menuTouch(up ? 2 : 3, cx, cy));
            invalidate();
        }
        if (statesVelocity != null) { statesVelocity.recycle(); statesVelocity = null; }
        statesTouch = false;
        statesScrolling = false;
    }

    /** Run (on the UI thread) each time the menu goes from open to closed:
     *  the one-screen overlay puts itself away and lets the game go on. */
    Runnable onMenuClosed = null;

    private void setMenuImage(int[] argb) {
        final boolean was = menuImage != null;
        if (argb == null || argb.length < MENU_W * MENU_H) {
            menuImage = null;
        } else {
            try {
                menuImage = Bitmap.createBitmap(argb, MENU_W, MENU_H, Bitmap.Config.ARGB_8888);
            } catch (IllegalArgumentException e) { menuImage = null; }
        }
        if (was && menuImage == null && onMenuClosed != null) post(onMenuClosed);
    }

    boolean menuShown() { return menuImage != null; }

    /** From the second-screen thread: the pad moved through the menu. */
    public void setMenuImageFromNative(int[] argb) {
        setMenuImage(argb);
        postInvalidate();
    }

    private void drawMenu(Canvas c) {
        final float w = getWidth(), h = getHeight();
        int k = (int) Math.floor(Math.min(w / MENU_W, h / MENU_H));
        if (k < 1) k = 1;
        menuScale = k;
        final float dw = MENU_W * k, dh = MENU_H * k;
        menuRect.set((w - dw) * 0.5f, 0f, (w + dw) * 0.5f, dh);   // at the top, as the sheet
        final Bitmap m = menuImage;
        if (m != null) {
            c.drawColor(Color.BLACK);
            c.drawBitmap(m, null, menuRect, blit);
        }
        drawStatesGrid(c, m, k);
        // The opening plays on both screens with nothing over it: no zone, no
        // button (it comes back with the title).
        for (int b = 1; b < ZONE_BUTTONS; ++b) zoneRects[b].setEmpty();   // only the zone has them
        for (int b = 0; b < ZONE_BUTTONS; ++b) zoneVisRects[b].setEmpty();
        zoneRect.setEmpty();
        if (sceneKind == 1 && m == null) {
            buttonRect.setEmpty();
            return;
        }
        // In a shop too (the user asked to drop the bottom bar to give the
        // portraits more room): the list has the whole screen.
        if (listShop && listImage != null && m == null) {
            buttonRect.setEmpty();
            return;
        }
        // The band under the page: a zone in the frame of the game's windows
        // (the SUPPLY menu's), the whole band tall, with the button inside it.
        final int zoneH = (int) Math.floor((h - dh) / k);
        if (zoneH >= BUTTON) {
            int world = 0;   // the world buttons: 0 none, 1 on (+4/+8 Training/Shop greyed), 2 greyed
            try { world = SecondScreen.menuWorld(); } catch (Throwable t) { /* not ready */ }
            final int pressed = buttonDown && buttonInside ? buttonId : -1;
            final int key = (zoneH * 32 + world) * 16 + pressed + 1;
            Bitmap zb = zoneImages.get(key);
            if (zb == null) {
                try {
                    final int[] z = SecondScreen.menuZone(zoneH, pressed, world);
                    if (z != null && z.length >= MENU_W * zoneH) {
                        zb = Bitmap.createBitmap(z, MENU_W, zoneH, Bitmap.Config.ARGB_8888);
                        zoneImages.put(key, zb);
                    }
                } catch (Throwable t) { /* native not ready yet */ }
            }
            if (zb != null) {
                final float zl = menuRect.left, zt = dh;
                zoneRect.set(zl, zt, zl + MENU_W * k, zt + zoneH * k);
                c.drawBitmap(zb, null, zoneRect, blit);
                final float by = zt + ((zoneH - BUTTON) / 2) * k;
                for (int b = 0; b < ZONE_BUTTONS; ++b) {
                    final float bx = zl + zoneButtonX(b) * k;
                    if (zoneLive(b, world)) zoneRects[b].set(bx, by, bx + BUTTON * k, by + BUTTON * k);
                    else zoneRects[b].setEmpty();
                    if (b == ZONE_OPTION || (world & 3) != 0)
                        zoneVisRects[b].set(bx, by, bx + BUTTON * k, by + BUTTON * k);
                    else zoneVisRects[b].setEmpty();
                }
                buttonRect.set(zoneRects[ZONE_OPTION]);
                if (m != null) drawHand(c, k);
                return;
            }
        }
        if (buttonImage == null) {
            try {
                final int[] b = SecondScreen.menuButton();
                if (b != null && b.length >= BUTTON * BUTTON)
                    buttonImage = Bitmap.createBitmap(b, BUTTON, BUTTON, Bitmap.Config.ARGB_8888);
            } catch (Throwable t) { /* native not ready yet */ }
        }
        if (buttonImage != null) {
            final float bs = BUTTON * BUTTON_SCALE, margin = 8f;
            buttonRect.set(w - bs - margin, h - bs - margin, w - margin, h - margin);
            c.drawBitmap(buttonImage, null, buttonRect, blit);
            if (m != null) drawHand(c, BUTTON_SCALE);
        }
    }

    /** The hand left of the button, pointing at it, at scale k. */
    private void drawHand(Canvas c, float k) {
        if (handImage == null) {
            try {
                final int[] p = SecondScreen.menuHand();
                if (p != null && p.length >= HAND_W * HAND_H)
                    handImage = Bitmap.createBitmap(p, HAND_W, HAND_H, Bitmap.Config.ARGB_8888);
            } catch (Throwable t) { /* native not ready yet */ }
            if (handImage == null) return;
        }
        final float x = buttonRect.left - (HAND_W + HAND_GAP) * k;
        final float y = buttonRect.centerY() - HAND_H * k * 0.5f;
        c.drawBitmap(handImage, null, new RectF(x, y, x + HAND_W * k, y + HAND_H * k), blit);
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent e) {
        final float x = e.getX(), y = e.getY();
        final int act = e.getActionMasked();
        if (act == android.view.MotionEvent.ACTION_UP || act == android.view.MotionEvent.ACTION_CANCEL) {
            removeCallbacks(longPress);   // released before it became the card
            helpPending = -1;
        }
        if (act == android.view.MotionEvent.ACTION_DOWN) {
            int vis = buttonRect.contains(x, y) ? ZONE_OPTION : -1;
            for (int b = 1; b < ZONE_BUTTONS && vis < 0; ++b)
                if (zoneVisRects[b].contains(x, y)) vis = b;
            if (helpButton >= 0) {
                // The card is up: another button switches it, anything else
                // (its own button included) closes it. The tap does no more.
                if (vis >= 0 && vis != helpButton) showHelp(vis);
                else closeHelp();
                zoneSwallow = true;
                return true;
            }
            int id = buttonRect.contains(x, y) ? ZONE_OPTION : -1;
            for (int b = 1; b < ZONE_BUTTONS && id < 0; ++b)
                if (zoneRects[b].contains(x, y)) id = b;
            if (vis >= 0) {   // held, it becomes the card (greyed buttons too)
                helpPending = vis;
                postDelayed(longPress, LONG_PRESS_MS);
            }
            if (id >= 0) {
                buttonDown = true;
                buttonId = id;
                buttonInside = true;
                invalidate();   // pushed in
                return true;
            }
            zoneSwallow = zoneRect.contains(x, y);
        }
        if (zoneSwallow) {   // the bar's, off a live button: nothing under it
            if (act == android.view.MotionEvent.ACTION_UP || act == android.view.MotionEvent.ACTION_CANCEL)
                zoneSwallow = false;
            return true;
        }
        if (buttonDown) {
            final RectF r = buttonId == ZONE_OPTION ? buttonRect : zoneRects[buttonId];
            if (act == android.view.MotionEvent.ACTION_UP) {
                buttonDown = false;
                if (r.contains(x, y)) {
                    // Each with the game's sound.
                    if (buttonId == ZONE_SAVE) SecondScreen.worldSave();
                    else if (buttonId == ZONE_LOAD) SecondScreen.worldLoad();
                    else if (buttonId >= ZONE_ITEM) SecondScreen.worldItem(buttonId - ZONE_ITEM);
                    else setMenuImage(SecondScreen.menuToggle());
                }
                invalidate();
            } else if (act == android.view.MotionEvent.ACTION_CANCEL) {
                buttonDown = false;
                invalidate();
            } else if (act == android.view.MotionEvent.ACTION_MOVE) {
                final boolean inside = r.contains(x, y);
                if (inside != buttonInside) { buttonInside = inside; invalidate(); }
                if (!inside) { removeCallbacks(longPress); helpPending = -1; }   // slid off
            }
            return true;
        }
        if (menuImage == null) {
            if (sceneStrip != null) return true;   // the opening scenes: only the button
            if (editTouching || (act == android.view.MotionEvent.ACTION_DOWN && editImage != null)) {
                editTouchEvent(act, x, y);
                return true;
            }
            if (listTouch || (act == android.view.MotionEvent.ACTION_DOWN && listOnly()))
                listTouchEvent(e, act, x, y);
            else
                sheetTouchEvent(e, act, x, y);
            return true;
        }
        if (statesTouch || (act == android.view.MotionEvent.ACTION_DOWN && statesRows >= 0)) {
            statesTouchEvent(e, act, x, y);
            return true;
        }
        int a;
        switch (act) {
            case android.view.MotionEvent.ACTION_DOWN: a = 0; break;
            case android.view.MotionEvent.ACTION_MOVE: a = 1; break;
            case android.view.MotionEvent.ACTION_UP: a = 2; break;
            case android.view.MotionEvent.ACTION_CANCEL: a = 3; break;
            default: return true;
        }
        setMenuImage(SecondScreen.menuTouch(a, (x - menuRect.left) / menuScale,
                                               (y - menuRect.top) / menuScale));
        invalidate();
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        drawContent(c);
        drawMenu(c);
        drawBarHelp(c);
    }

    // ── The editor page (native kol_edit_panel) ─────────────────────────
    // While Debug > Tile Info is on, the editor's page takes the sheet's
    // place, at its scale and at the top; every touch on it goes to native in
    // canvas pixels (the zone and the menu keep theirs).
    private volatile Bitmap editImage = null;
    private float editLeft = 0f, editTop = 0f, editK = 1f;
    private boolean editTouching = false;

    /** From the second-screen thread: the page, or null once it is off. */
    public void setEditPanel(int[] argb) {
        Bitmap b = null;
        if (argb != null && argb.length >= MENU_W * MENU_H) {
            try { b = Bitmap.createBitmap(argb, MENU_W, MENU_H, Bitmap.Config.ARGB_8888); }
            catch (IllegalArgumentException e) { b = null; }
        }
        editImage = b;
        postInvalidate();
    }

    private void drawEdit(Canvas c, Bitmap img) {
        c.drawColor(Color.BLACK);
        final float w = getWidth(), h = getHeight();
        int k = (int) Math.floor(Math.min(w / MENU_W, h / MENU_H));
        if (k < 1) k = 1;
        editK = k;
        editLeft = (w - MENU_W * k) * 0.5f;
        editTop = 0f;
        c.drawBitmap(img, null, new RectF(editLeft, editTop, editLeft + MENU_W * k, editTop + MENU_H * k), blit);
    }

    private void editTouchEvent(int act, float x, float y) {
        int a;
        switch (act) {
            case android.view.MotionEvent.ACTION_DOWN: a = 0; editTouching = true; break;
            case android.view.MotionEvent.ACTION_MOVE: a = 1; break;
            case android.view.MotionEvent.ACTION_UP: a = 2; editTouching = false; break;
            case android.view.MotionEvent.ACTION_CANCEL: a = 3; editTouching = false; break;
            default: return;
        }
        try { SecondScreen.editTouch(a, (x - editLeft) / editK, (y - editTop) / editK); }
        catch (Throwable t) { /* native not ready */ }
    }

    // ── The opening scenes (native kol_scene_strip) ─────────────────────
    // The opening's mountain or the title's clouds carrying on below the top
    // screen, over the whole second screen at the pages' scale, centred
    // across; the zone lies over its foot.
    private Bitmap sceneStrip = null;           // UI thread only
    private int sceneKind = 0;                   // 1 the opening, 2 the title

    public void setSceneStrip(final int[] argb, final int w, final int h, final int scene) {
        post(() -> {
            sceneKind = argb == null ? 0 : scene;
            if (argb == null || w <= 0 || h <= 0 || argb.length < w * h) {
                sceneStrip = null;
            } else {
                Bitmap b = sceneStrip;
                if (b == null || b.getWidth() != w || b.getHeight() != h) {
                    b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    sceneStrip = b;
                }
                b.setPixels(argb, 0, w, 0, 0, w, h);
            }
            invalidate();
        });
    }

    private void drawSceneStrip(Canvas c, Bitmap img) {
        c.drawColor(Color.BLACK);
        final float w = getWidth(), h = getHeight();
        int k = (int) Math.floor(Math.min(w / MENU_W, h / MENU_H));
        if (k < 1) k = 1;
        final int rows = Math.min(img.getHeight(), (int) Math.ceil(h / k));
        final float left = (w - img.getWidth() * k) * 0.5f;
        c.drawBitmap(img, new Rect(0, 0, img.getWidth(), rows),
                     new RectF(left, 0f, left + img.getWidth() * k, rows * k), blit);
    }

    /** The page: the opening scenes, the sheet (with the list under it while
     *  the player browses the map), the list alone, or nothing yet. Below it,
     *  the zone and the menu (drawMenu). */
    private void drawContent(Canvas c) {
        super.onDraw(c);
        final Bitmap img = sheetImage;
        final Bitmap edit = editImage;
        if (sceneStrip != null) drawSceneStrip(c, sceneStrip);
        else if (edit != null) drawEdit(c, edit);
        else if (img != null) drawSheetArea(c, img);
        else if (listImage != null) {
            // The list alone: the card is away, so a sheet that comes later
            // slides up from below rather than starting in place.
            cardPos = 1f;
            cardAnimStart = -1;
            drawList(c);
        }
        else c.drawColor(Color.BLACK);
        drawAutoWin(c);
    }
}
