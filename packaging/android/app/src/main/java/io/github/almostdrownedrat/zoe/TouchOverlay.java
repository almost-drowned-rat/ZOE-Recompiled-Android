// Zone of the Enders Recompiled - the touch controls (Android). Agent L, 2026-11-21, from the Orta Recompiled
// Android app's TouchOverlay (O 2026-11-11) with a DualShock 2's buttons instead of the Xbox pad's.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A transparent view over SDL's surface: a floating left stick (it centres where the thumb lands in the left part),
// Cross / Circle / Square / Triangle right, L2 / R2 in the top corners with L1 / R1 below them, SELECT and START
// bottom centre, a fixed right stick left of the face buttons. L3, R3 and the D-pad are there but hidden until shown
// in the BACK menu. Its state goes to native code as one virtual SDL gamepad (jni/app/touchpad.c, libzoe_app.so),
// so the game sees an ordinary pad. Drawn with Canvas - nothing in the game's Vulkan path. Touches outside the
// controls fall through to SDL. A real gamepad's input hides it (mode AUTO), a touch shows it again. Layout, hidden
// buttons, opacity and the mode are per device (SharedPreferences); the BACK menu (ZoeActivity) edits them.
package io.github.almostdrownedrat.zoe;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

public class TouchOverlay extends View {
    // virtual pad button bits (jni/app/touchpad.c maps them to SDL gamepad buttons): the face buttons by position
    // (Cross = SOUTH, Circle = EAST, Square = WEST, Triangle = NORTH), L1 / R1 the shoulders, L2 / R2 the triggers,
    // SELECT = BACK; STICK = the floating left stick, RSTICK = the fixed right stick
    static final int CROSS = 0, CIRCLE = 1, SQUARE = 2, TRIANGLE = 3, L1 = 4, R1 = 5, START = 6, L2 = 7, R2 = 8,
                     STICK = 9, SELECT = 10, L3 = 11, R3 = 12, UP = 13, DOWN = 14, LEFT = 15, RIGHT = 16, RSTICK = 17,
                     N = 18;
    static final String[] NAMES = { "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1", "START", "L2", "R2",
                                    "LEFT STICK", "SELECT", "L3", "R3", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT",
                                    "D-PAD RIGHT", "RIGHT STICK" };
    private static final String[] LABELS = { "", "", "", "", "L1", "R1", "START", "L2", "R2", "",
                                              "SELECT", "L3", "R3", "▲", "▼", "◀", "▶", "" };
    /** hidden unless the player shows it: the stick clicks and the D-pad (the right stick stays: ZOE's camera) */
    private static boolean hiddenByDefault(int i) { return i == L3 || i == R3 || (i >= UP && i <= RIGHT); }
    private static boolean centred(int i) { return i == START || i == SELECT; }     // x 0 = the screen's centre
    private static boolean face(int i) { return i <= TRIANGLE; }
    // the PlayStation symbols' colours: Cross blue, Circle red, Square pink, Triangle green
    private static final int[] FACE_RGB = { 0x7CA8FF, 0xFF6A6A, 0xF28CD0, 0x3EDBA8 };
    static final int MODE_AUTO = 0, MODE_ALWAYS = 1, MODE_OFF = 2;

    static native void nativeTouchPad(int buttons, float lx, float ly, float lt, float rt, float rx, float ry);

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                        text = new Paint(Paint.ANTI_ALIAS_FLAG), sym = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path tri = new Path();
    private final float dp;
    private final SharedPreferences prefs;
    // control centres in dp from an anchor corner: x >= 0 from the left, x < 0 from the right; same for y (bottom)
    private final float[] cx = new float[N], cy = new float[N], r = new float[N];
    private final boolean[] hidden = new boolean[N];
    private final float[] sc = new float[N];          // per-button size (pinch in the editor), 1 = default
    private float gscale = 1f;                        // all buttons (BACK menu > Button size)
    private float sens = 1f;                          // stick sensitivity: full deflection after 1/sens of the radius
    private int mode = MODE_AUTO, opacity = 50;
    private float pinchStart, pinchScale;             // editor: two fingers on a button resize it
    private boolean padActive, editing;
    // touch state: which pointer holds which control; the stick's origin
    private final int[] owner = new int[N];
    private float stickOx, stickOy, stickX, stickY, rstickX, rstickY;
    private int dragControl = -1;
    private float dragDx, dragDy;

    public TouchOverlay(Context ctx) {
        super(ctx);
        dp = ctx.getResources().getDisplayMetrics().density;
        prefs = ctx.getSharedPreferences("touch", Context.MODE_PRIVATE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        line.setStyle(Paint.Style.STROKE);
        sym.setStyle(Paint.Style.STROKE);
        sym.setStrokeCap(Paint.Cap.ROUND);
        sym.setStrokeJoin(Paint.Join.ROUND);
        for (int i = 0; i < N; i++) owner[i] = -1;
        load();
    }

    private void defaults() {
        float[][] d = {                      // x, y (dp; negative = from the right / bottom), radius
            { -110, -70, 34 },  /* CROSS */   { -50, -130, 34 },   /* CIRCLE */
            { -170, -130, 34 }, /* SQUARE */  { -110, -190, 34 },  /* TRIANGLE */
            { 70, 112, 26 },    /* L1 */      { -70, 112, 26 },    /* R1 */
            { 60, -36, 26 },    /* START (x from the centre) */
            { 70, 48, 34 },     /* L2 */      { -70, 48, 34 },     /* R2 */
            { 150, -150, 70 },  /* STICK: the rest place of the floating stick */
            { -60, -36, 26 },   /* SELECT (x from the centre) */
            { 150, -270, 24 },  /* L3 */      { -300, -200, 24 },  /* R3 */
            { 250, -170, 22 },  /* UP */      { 250, -70, 22 },    /* DOWN */
            { 200, -120, 22 },  /* LEFT */    { 300, -120, 22 },   /* RIGHT */
            { -300, -100, 50 }, /* RSTICK: a fixed stick */
        };
        for (int i = 0; i < N; i++) { cx[i] = d[i][0]; cy[i] = d[i][1]; r[i] = d[i][2]; }
    }

    private void load() {
        defaults();
        for (int i = 0; i < N; i++) {
            cx[i] = prefs.getFloat("x" + i, cx[i]);
            cy[i] = prefs.getFloat("y" + i, cy[i]);
            hidden[i] = prefs.getBoolean("h" + i, hiddenByDefault(i));
            sc[i] = prefs.getFloat("s" + i, 1f);
        }
        gscale = prefs.getFloat("gscale", 1f);
        sens = prefs.getFloat("sens", 1f);
        mode = prefs.getInt("mode", MODE_AUTO);
        opacity = prefs.getInt("opacity", 50);
        updateVisibility();
    }

    void save() {
        SharedPreferences.Editor e = prefs.edit();
        for (int i = 0; i < N; i++) {
            e.putFloat("x" + i, cx[i]); e.putFloat("y" + i, cy[i]); e.putBoolean("h" + i, hidden[i]); e.putFloat("s" + i, sc[i]);
        }
        e.putFloat("gscale", gscale).putFloat("sens", sens).putInt("mode", mode).putInt("opacity", opacity).apply();
    }

    void resetLayout() {
        defaults();
        gscale = 1f;
        for (int i = 0; i < N; i++) { hidden[i] = hiddenByDefault(i); sc[i] = 1f; }
        save();
        invalidate();
    }

    int getMode() { return mode; }
    void setMode(int m) { mode = m; save(); updateVisibility(); }
    int getOpacity() { return opacity; }
    float getScale() { return gscale; }
    float getSensitivity() { return sens; }
    void setSensitivity(float s) { sens = s; save(); }
    void setScale(float s) { gscale = s; save(); invalidate(); }
    /** the radius of control i in pixels: its base size x its own (pinch) x all buttons (menu) */
    private float rad(int i) { return r[i] * sc[i] * gscale * dp; }
    void setOpacity(int o) { opacity = o; save(); invalidate(); }
    boolean isHidden(int i) { return hidden[i]; }
    void setHidden(int i, boolean h) { hidden[i] = h; save(); invalidate(); }
    void setEditing(boolean e) { editing = e; updateVisibility(); invalidate(); }
    boolean isEditing() { return editing; }

    /** A real gamepad was used: AUTO hides the controls (and releases everything). */
    void onGamepadInput() {
        if (padActive) return;
        padActive = true;
        release();
        updateVisibility();
    }

    private boolean gameRunning;

    /** ZoeActivity.onCreate: the game starts right away (no launcher in front of it yet). */
    void setGameRunning() {
        gameRunning = true;
        // a gamepad already connected (the Odin 2 Portal's own controls): AUTO starts hidden
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d != null && !d.isVirtual()
                && (d.getSources() & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD)
                padActive = true;
        }
        updateVisibility();
    }
    boolean isGameRunning() { return gameRunning; }

    private void updateVisibility() {
        boolean show = gameRunning && (editing || mode == MODE_ALWAYS || (mode == MODE_AUTO && !padActive));
        setVisibility(show ? VISIBLE : INVISIBLE);
    }

    private float px(int i) { float w = getWidth(); return centred(i) ? w / 2 + cx[i] * dp : cx[i] >= 0 ? cx[i] * dp : w + cx[i] * dp; }
    private float py(int i) { float h = getHeight(); return cy[i] >= 0 ? cy[i] * dp : h + cy[i] * dp; }

    private int hit(float x, float y) {
        for (int i = 0; i < N; i++) {
            if (hidden[i] && !editing) continue;
            if (i == STICK) continue;
            float dx = x - px(i), dy = y - py(i), rr = rad(i) + (i == RSTICK ? 0 : 12 * dp);
            if (dx * dx + dy * dy <= rr * rr) return i;
        }
        // the stick zone: the left 40 % of the screen, lower 75 % (floating), or its rest place in edit mode
        if (!hidden[STICK] || editing) {
            if (editing) {
                float dx = x - px(STICK), dy = y - py(STICK), rr = rad(STICK);
                if (dx * dx + dy * dy <= rr * rr) return STICK;
            } else if (x < getWidth() * 0.4f && y > getHeight() * 0.25f) {
                return STICK;
            }
        }
        return -1;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), idx = e.getActionIndex(), id = e.getPointerId(idx);
        if (padActive && mode == MODE_AUTO) { padActive = false; updateVisibility(); }
        if (editing) return editTouch(e, a, idx);
        switch (a) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int c = hit(e.getX(idx), e.getY(idx));
                if (c < 0) return a != MotionEvent.ACTION_DOWN;    // not ours: SDL gets the gesture
                owner[c] = id;
                if (c == STICK) { stickOx = e.getX(idx); stickOy = e.getY(idx); stickX = 0; stickY = 0; }
                if (c == RSTICK) moveRStick(e.getX(idx), e.getY(idx));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int p = 0; p < e.getPointerCount(); p++) {
                    if (owner[STICK] == e.getPointerId(p)) {
                        float max = rad(STICK) / sens;    // higher sensitivity: full deflection with a shorter thumb move
                        float dx = (e.getX(p) - stickOx) / max, dy = (e.getY(p) - stickOy) / max;
                        float len = (float) Math.sqrt(dx * dx + dy * dy);
                        if (len > 1) { dx /= len; dy /= len; }
                        stickX = dx; stickY = dy;
                    }
                    if (owner[RSTICK] == e.getPointerId(p)) moveRStick(e.getX(p), e.getY(p));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < N; i++) {
                    if (owner[i] == id || a == MotionEvent.ACTION_CANCEL) {
                        owner[i] = -1;
                        if (i == STICK) { stickX = 0; stickY = 0; }
                        if (i == RSTICK) { rstickX = 0; rstickY = 0; }
                    }
                }
                break;
        }
        send();
        invalidate();
        return true;
    }

    private boolean editTouch(MotionEvent e, int a, int idx) {
        float x = e.getX(idx), y = e.getY(idx);
        if (a == MotionEvent.ACTION_DOWN) {
            dragControl = hit(x, y);
            pinchStart = 0;
            if (dragControl >= 0) { dragDx = px(dragControl) - x; dragDy = py(dragControl) - y; }
        } else if (a == MotionEvent.ACTION_POINTER_DOWN && dragControl >= 0 && e.getPointerCount() == 2) {
            pinchStart = (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1));   // second finger: resize
            pinchScale = sc[dragControl];
        } else if (a == MotionEvent.ACTION_MOVE && dragControl >= 0 && pinchStart > 0 && e.getPointerCount() >= 2) {
            float d = (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1));
            sc[dragControl] = Math.max(0.5f, Math.min(2.5f, pinchScale * d / pinchStart));
            invalidate();
        } else if (a == MotionEvent.ACTION_MOVE && dragControl >= 0 && pinchStart == 0) {
            float nx = (e.getX() + dragDx), ny = (e.getY() + dragDy), w = getWidth(), h = getHeight();
            int c = dragControl;
            if (centred(c)) cx[c] = (nx - w / 2) / dp;
            else cx[c] = nx < w / 2 ? nx / dp : (nx - w) / dp;
            cy[c] = ny < h / 2 ? ny / dp : (ny - h) / dp;
            invalidate();
        } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
            dragControl = -1;
            save();
        }
        return true;
    }

    void release() {                                   // also SideMenu opening: no button stays held
        for (int i = 0; i < N; i++) owner[i] = -1;
        stickX = stickY = rstickX = rstickY = 0;
        send();
        invalidate();
    }

    /** the fixed right stick: the deflection from its centre (full at its radius / sensitivity) */
    private void moveRStick(float x, float y) {
        float max = rad(RSTICK) / sens, dx = (x - px(RSTICK)) / max, dy = (y - py(RSTICK)) / max;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > 1) { dx /= len; dy /= len; }
        rstickX = dx; rstickY = dy;
    }

    private void send() {
        int b = 0;
        for (int i = 0; i < N; i++)
            if (owner[i] >= 0 && i != L2 && i != R2 && i != STICK && i != RSTICK) b |= 1 << i;
        try {
            nativeTouchPad(b, stickX, stickY, owner[L2] >= 0 ? 1f : 0f, owner[R2] >= 0 ? 1f : 0f, rstickX, rstickY);
        } catch (UnsatisfiedLinkError ignored) { /* libzoe_app not loaded */ }
    }

    /** Cross, Circle, Square, Triangle as the PlayStation's symbols (outlines in their colours). */
    private void drawSymbol(Canvas c, int i, float x, float y, float rr, int al, boolean down) {
        int rgb = FACE_RGB[i];
        sym.setColor(down ? Color.argb(al, 20, 20, 20)
                          : Color.argb(al, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
        sym.setStrokeWidth(Math.max(2 * dp, rr * 0.11f));
        float s = rr * 0.42f;
        switch (i) {
            case CROSS:
                c.drawLine(x - s, y - s, x + s, y + s, sym);
                c.drawLine(x - s, y + s, x + s, y - s, sym);
                break;
            case CIRCLE:
                c.drawCircle(x, y, s * 1.05f, sym);
                break;
            case SQUARE:
                c.drawRect(x - s * 0.9f, y - s * 0.9f, x + s * 0.9f, y + s * 0.9f, sym);
                break;
            case TRIANGLE:
                tri.reset();
                tri.moveTo(x, y - s * 1.1f);
                tri.lineTo(x + s * 1.05f, y + s * 0.7f);
                tri.lineTo(x - s * 1.05f, y + s * 0.7f);
                tri.close();
                c.drawPath(tri, sym);
                break;
            default: break;
        }
    }

    @Override
    protected void onDraw(Canvas c) {
        int al = editing ? 200 : opacity * 255 / 100;
        line.setStrokeWidth(2 * dp);
        for (int i = 0; i < N; i++) {
            if (hidden[i] && !editing) continue;
            float x = px(i), y = py(i), rr = rad(i);
            boolean down = owner[i] >= 0;
            if (i == STICK) {
                float ox = owner[STICK] >= 0 ? stickOx : x, oy = owner[STICK] >= 0 ? stickOy : y;
                fill.setColor(Color.argb(al / 3, 255, 255, 255));
                c.drawCircle(ox, oy, rr, fill);
                line.setColor(Color.argb(hidden[i] ? al / 3 : al, 255, 255, 255));
                c.drawCircle(ox, oy, rr, line);
                fill.setColor(Color.argb(al, 255, 255, 255));
                c.drawCircle(ox + stickX * rr, oy + stickY * rr, rr * 0.45f, fill);
                continue;
            }
            if (i == RSTICK) {
                fill.setColor(Color.argb(al / 3, 255, 255, 255));
                c.drawCircle(x, y, rr, fill);
                line.setColor(Color.argb(hidden[i] ? al / 3 : al, 255, 255, 255));
                c.drawCircle(x, y, rr, line);
                fill.setColor(Color.argb(al, 255, 255, 255));
                c.drawCircle(x + rstickX * rr, y + rstickY * rr, rr * 0.45f, fill);
                continue;
            }
            fill.setColor(down ? Color.argb(al, 255, 255, 255) : Color.argb(al / 2, 40, 40, 40));
            line.setColor(Color.argb(hidden[i] ? al / 3 : al, 255, 255, 255));
            if (i == L1 || i == R1 || i == L2 || i == R2 || i == START || i == SELECT || i == L3 || i == R3) {
                RectF rc = new RectF(x - rr * 1.4f, y - rr * 0.7f, x + rr * 1.4f, y + rr * 0.7f);
                c.drawRoundRect(rc, rr * 0.5f, rr * 0.5f, fill);
                c.drawRoundRect(rc, rr * 0.5f, rr * 0.5f, line);
            } else {
                c.drawCircle(x, y, rr, fill);
                c.drawCircle(x, y, rr, line);
            }
            if (face(i)) {
                drawSymbol(c, i, x, y, rr, al, down);
                continue;
            }
            // SELECT / START are wide words: a smaller type so they fit their key
            text.setTextSize((centred(i) ? 12 : 15) * dp * Math.min(1.5f, sc[i] * gscale));
            text.setColor(down ? Color.argb(al, 0, 0, 0) : Color.argb(al, 255, 255, 255));
            c.drawText(LABELS[i], x, y - (text.descent() + text.ascent()) / 2, text);
        }
        if (editing) {
            text.setColor(Color.WHITE);
            text.setTextSize(16 * dp);
            c.drawText("Drag a button to move it, pinch it with two fingers to resize - BACK when done",
                       getWidth() / 2f, 40 * dp, text);
        }
    }
}
