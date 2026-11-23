// Zone of the Enders Recompiled for Android: SDL's activity, the game in libmain.so (its main() is SDL_main).
// Agent L, 2026-11-21 - after the Orta Recompiled Android app's OrtaActivity: the BACK menu (SideMenu), the touch
// controls (TouchOverlay + jni/app/touchpad.c in libzoe_app.so), saves / log / boot animation / graphics driver.
// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.almostdrownedrat.zoe;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.os.Build;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

public class ZoeActivity extends SDLActivity {
    private static final String TAG = "ZOE";
    private static final String GAME_TITLE = "Zone of the Enders";

    /** The game the setup built on this device (M2): <files>/bin/libZOE_game.so - when it is there, SDL runs its
     *  SDL_main instead of the APK's libmain.so. (An app may load code from its own private files; it may not exec.) */
    private File deviceGame() { return new File(getFilesDir(), "bin/libZOE_game.so"); }

    @Override
    protected String[] getLibraries() {
        // libzoe_app.so (the touch controls' virtual gamepad) links libSDL3.so, so it loads after it
        if (deviceGame().isFile()) return new String[] { "SDL3", "zoe_app" };
        return new String[] { "SDL3", "zoe_app", "main" };
    }

    @Override
    protected String getMainSharedObject() {
        File g = deviceGame();
        if (g.isFile()) {
            Log.i(TAG, "game: the device-built " + g + " (" + g.length() + " bytes)");
            return g.getAbsolutePath();
        }
        return super.getMainSharedObject();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before SDL loads the libraries: the game's Vulkan is libvulkan_zoe.so (the system driver, or the player's
        // Turnip in files/turnip/ through libadrenotools - vulkan_zoe.c reads XR_NATIVE_DIR / XR_HOST_ROOT)
        try {
            Os.setenv("XR_NATIVE_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("XR_HOST_ROOT", getFilesDir().getAbsolutePath(), true);
            Os.setenv("SDL_VULKAN_LIBRARY", "libvulkan_zoe.so", true);
            // Turnip draws straight into memory, not in tiles (the system driver does not read it): the GS's many
            // small draws and render passes cost the tiling more than they save - ZOE's boss fight on an Odin 2
            // 36.5 -> 55.3 PS2 frames a second, the hangar ~+10 %. dev_env.txt may set another (TU_DEBUG=).
            Os.setenv("TU_DEBUG", "sysmem", false);
        } catch (ErrnoException e) {
            android.util.Log.w("ZOE", "setenv: " + e);
        }
        // measurements without a rebuild (as Orta's loader): <external files>/dev_env.txt, KEY=VALUE a line, set for
        // the game (lines starting with # ignored) - a developer's aid; a player never has the file
        java.io.File devEnv = new java.io.File(getExternalFilesDir(null), "dev_env.txt");
        if (devEnv.isFile()) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(devEnv))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    int eq = line.indexOf('=');
                    if (line.isEmpty() || line.startsWith("#") || eq <= 0) continue;
                    Os.setenv(line.substring(0, eq).trim(), line.substring(eq + 1).trim(), true);
                    android.util.Log.i("ZOE", "dev_env.txt: " + line);
                }
            } catch (java.io.IOException | ErrnoException e) {
                android.util.Log.w("ZOE", "dev_env.txt: " + e);
            }
        }
        startRunLog();
        applyPendingSaves();            // a saves import staged before the restart: swapped in before the game runs
        prepareUserDirs();
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        // BACK: with targetSdk 36 the back gesture / key reaches an app only through OnBackInvokedCallback
        // (predictive back; onBackPressed and KEYCODE_BACK are no longer delivered on Android 13+). BACK opens the
        // menu; in the open menu it goes back one level; in the touch layout editor it ends editing.
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, () -> {
                    Log.i(TAG, "[BACK] back gesture / key");
                    onBackMenu();
                });
        }
        // the touch controls over SDL's surface (TouchOverlay.java)
        mOverlay = new TouchOverlay(this);
        mLayout.addView(mOverlay, new android.widget.RelativeLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        // the BACK menu: a panel sliding in from the left (SideMenu.java), above the touch controls
        mMenu = new SideMenu(this);
        mMenu.setOnClosed(this::immersive);
        mLayout.addView(mMenu, new android.widget.RelativeLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        // no launcher in front of the game (yet): the controls are there from the start
        mOverlay.setGameRunning();
    }

    @Override
    protected void onResume() {
        super.onResume();
        immersive();                // (back from Home / another app: the bars hidden again)
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    /** The display at 60 Hz where it offers it (the game's own rate - a PS2 game paces to 60): at the Odin 2's
     *  default 120 Hz the engine's pacing presented only every ~34 ms and the whole chain (EE, VIF1, GS) backed up
     *  behind it - 35-40 fps with an idle GPU (2026-11-21). The same resolution, the refresh closest to 60. */
    private void prefer60Hz() {
        android.view.Display d = getWindowManager().getDefaultDisplay();
        if (d == null) return;
        android.view.Display.Mode cur = d.getMode(), best = null;
        for (android.view.Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
            if (best == null || Math.abs(m.getRefreshRate() - 60f) < Math.abs(best.getRefreshRate() - 60f)) best = m;
        }
        if (best == null || Math.abs(best.getRefreshRate() - 60f) > 1f) return;
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        if (lp.preferredDisplayModeId != best.getModeId()) {
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
            android.util.Log.i("ZOE", "display mode " + best.getModeId() + " (" + best.getRefreshRate() + " Hz)");
        }
    }

    private void immersive() {
        prefer60Hz();
        // edge to edge, the bars' backgrounds transparent: a hidden status bar's black background could stay over
        // the picture's top (Odin 2, some starts - the FPS counter under it)
        getWindow().setStatusBarColor(android.graphics.Color.TRANSPARENT);
        getWindow().setNavigationBarColor(android.graphics.Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    // ---- Input: the BACK menu takes every key while open; a real gamepad hides the touch controls (AUTO) -------

    private TouchOverlay mOverlay;
    private SideMenu mMenu;
    private boolean mSelectHeld, mStartHeld;    // a gamepad's SELECT + START together open the menu too

    private static native void nativeTouchPadEnable(boolean on);

    /** The gamepads Android reports as built in (not USB / Bluetooth / a dock's), one name per line. */
    public static String builtInPadNames() {
        StringBuilder b = new StringBuilder();
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null || d.isVirtual() || d.isExternal() || !fromGamepad(d.getSources())) continue;
            b.append(d.getName()).append('\n');
        }
        return b.toString();
    }

    private static boolean fromGamepad(int source) {
        return (source & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
            || (source & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK;
    }

    private void gamepadUsed() {
        if (mOverlay == null || mOverlay.getVisibility() != android.view.View.VISIBLE
            || mOverlay.getMode() != TouchOverlay.MODE_AUTO || mOverlay.isEditing()) return;
        mOverlay.onGamepadInput();
        try { nativeTouchPadEnable(false); } catch (UnsatisfiedLinkError ignored) { }
    }

    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent e) {
        int k = e.getKeyCode();
        boolean pad = fromGamepad(e.getSource()), down = e.getAction() == android.view.KeyEvent.ACTION_DOWN;
        if (pad && k == android.view.KeyEvent.KEYCODE_BUTTON_SELECT) mSelectHeld = down;
        if (pad && k == android.view.KeyEvent.KEYCODE_BUTTON_START) mStartHeld = down;
        // the open menu takes every key (pad / keyboard), the game sees none; BACK itself comes through
        // OnBackInvokedCallback (onBackMenu) on Android 13+, as a key only on older ones. A gamepad's releases still
        // reach the game, so nothing held when the menu opened (SELECT + START) stays held there.
        if (mMenu != null && mMenu.isOpen()) {
            if (k == android.view.KeyEvent.KEYCODE_BACK && Build.VERSION.SDK_INT >= 33)
                return super.dispatchKeyEvent(e);
            mMenu.onKey(e);
            if (pad && !down) super.dispatchKeyEvent(e);
            return true;
        }
        // Android 12 and older: the system BACK key / gesture comes as KEYCODE_BACK, and SDL would eat it (it never
        // reaches onBackPressed) - so it opens the menu here, on the press like SideMenu.onKey's BACK (so the release
        // never reopens a menu its press closed); a gamepad's own BACK button stays the game's
        if (k == android.view.KeyEvent.KEYCODE_BACK && Build.VERSION.SDK_INT < 33 && !pad) {
            if (down && e.getRepeatCount() == 0) onBackMenu();
            return true;
        }
        // a gamepad's menu keys: the Guide / Home button (BUTTON_MODE), or SELECT + START held together
        if (pad && k == android.view.KeyEvent.KEYCODE_BUTTON_MODE) {
            if (down && e.getRepeatCount() == 0) onBackMenu();
            return true;
        }
        if (pad && down && e.getRepeatCount() == 0 && mSelectHeld && mStartHeld
            && (k == android.view.KeyEvent.KEYCODE_BUTTON_SELECT || k == android.view.KeyEvent.KEYCODE_BUTTON_START)) {
            onBackMenu();
            return true;
        }
        if (pad && k != android.view.KeyEvent.KEYCODE_BACK) gamepadUsed();
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(android.view.MotionEvent e) {
        if (mMenu != null && mMenu.isOpen() && fromGamepad(e.getSource())) return mMenu.onMotion(e);
        if (fromGamepad(e.getSource())) gamepadUsed();
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    public boolean dispatchTouchEvent(android.view.MotionEvent e) {
        // a touch brings the hidden (AUTO) controls back - the hidden view itself gets no touches
        if (mOverlay != null && (mMenu == null || !mMenu.isOpen()) && e.getActionMasked() == android.view.MotionEvent.ACTION_DOWN
            && mOverlay.getMode() == TouchOverlay.MODE_AUTO && mOverlay.getVisibility() != android.view.View.VISIBLE) {
            try { nativeTouchPadEnable(true); } catch (UnsatisfiedLinkError ignored) { }
            mOverlay.onTouchEvent(android.view.MotionEvent.obtain(e.getDownTime(), e.getEventTime(),
                android.view.MotionEvent.ACTION_CANCEL, e.getX(), e.getY(), 0));
        }
        return super.dispatchTouchEvent(e);
    }

    /** Android 12 and older: BACK as onBackPressed (13+: OnBackInvokedCallback). */
    @Override
    public void onBackPressed() {
        onBackMenu();
    }

    /** BACK: the menu as a panel from the left (SideMenu; the game keeps running behind it). BACK in the open menu
     *  goes back one level / closes it; in the touch layout editor it ends editing. */
    private void onBackMenu() {
        if (mOverlay == null || mMenu == null) return;
        if (mMenu.isOpen()) { mMenu.back(); return; }
        if (mOverlay.isEditing()) { mOverlay.setEditing(false); return; }
        mOverlay.release();
        mMenu.open(rootPage());
        immersive();
    }

    // ---- The menu (the maintainer's fixed order for all ports): Back to the game, Controls, Settings ----------

    private static String pct(float f) { return Math.round(f * 100) + " %"; }

    private SideMenu.Page rootPage() {
        return new SideMenu.Page() {
            public String title() { return GAME_TITLE; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                l.add(new SideMenu.Item("Back to the game") { @Override void run() { mMenu.close(); } });
                l.add(new SideMenu.Item("Controls") { @Override void run() { mMenu.push(controlsPage()); } });
                l.add(new SideMenu.Item("Settings") { @Override void run() { mMenu.push(settingsPage()); } });
                return l;
            }
        };
    }

    /** Controls: the touch buttons (automatic / always / off, move, size, hide single ones, opacity, stick, reset)
     *  and the connected controllers. */
    private SideMenu.Page controlsPage() {
        final String[] modes = { "Automatic", "Always", "Off" };
        return new SideMenu.Page() {
            public String title() { return "Controls"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                l.add(new SideMenu.Item("Touch buttons") {     // Automatic = hidden while a gamepad is used
                    @Override String value() { return modes[mOverlay.getMode()]; }
                    @Override void step(int d) {
                        int m = (mOverlay.getMode() + d + 3) % 3;
                        mOverlay.setMode(m);
                        try { nativeTouchPadEnable(m != TouchOverlay.MODE_OFF); } catch (UnsatisfiedLinkError ignored) { }
                    }
                });
                l.add(new SideMenu.Item("Controllers") { @Override void run() { mMenu.push(padPage()); } });
                l.add(new SideMenu.Item("Move and resize touch buttons") {
                    @Override void run() { mMenu.close(); mOverlay.setEditing(true); }
                });
                l.add(new SideMenu.Item("Button size") {       // all buttons, 50-150 %
                    @Override String value() { return pct(mOverlay.getScale()); }
                    @Override void step(int d) { mOverlay.setScale(Math.max(0.5f, Math.min(1.5f, mOverlay.getScale() + 0.25f * d))); }
                });
                l.add(new SideMenu.Item("Hide single touch buttons") { @Override void run() { mMenu.push(hidePage()); } });
                l.add(new SideMenu.Item("Opacity") {           // 25-100 %
                    @Override String value() { return mOverlay.getOpacity() + " %"; }
                    @Override void step(int d) { mOverlay.setOpacity(Math.max(25, Math.min(100, mOverlay.getOpacity() + 25 * d))); }
                });
                l.add(new SideMenu.Item("Stick sensitivity") {
                    @Override String value() { return pct(mOverlay.getSensitivity()); }
                    @Override void step(int d) { mOverlay.setSensitivity(Math.max(1f, Math.min(3f, mOverlay.getSensitivity() + 0.5f * d))); }
                });
                l.add(new SideMenu.Item("Reset touch layout") { @Override void run() { mOverlay.resetLayout(); mMenu.refresh(); } });
                return l;
            }
        };
    }

    /** The connected controllers as Android sees them (the game merges every one into its input; no swapping). */
    private SideMenu.Page padPage() {
        return new SideMenu.Page() {
            public String title() { return "Controllers"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                java.util.Set<String> seen = new java.util.HashSet<>();
                int n = 0;
                for (int id : android.view.InputDevice.getDeviceIds()) {
                    android.view.InputDevice d = android.view.InputDevice.getDevice(id);
                    if (d == null || d.isVirtual() || !fromGamepad(d.getSources())) continue;
                    if (!seen.add(d.getDescriptor())) continue;     // one pad can show as several input devices
                    l.add(new SideMenu.Item(d.getName() + "  (" + (d.isExternal() ? "external" : "built in") + ")"));
                    n++;
                }
                if (n == 0) l.add(new SideMenu.Item("No controller connected"));
                if (mOverlay.getMode() != TouchOverlay.MODE_OFF)
                    l.add(new SideMenu.Item("ZOE touch controls  (touch"
                        + (mOverlay.getVisibility() == View.VISIBLE ? ")" : ", hidden while a controller is used)")));
                return l;
            }
        };
    }

    private SideMenu.Page hidePage() {
        return new SideMenu.Page() {
            public String title() { return "Hide single touch buttons"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                for (int i = 0; i < TouchOverlay.N; i++) {
                    final int b = i;
                    l.add(new SideMenu.Item(TouchOverlay.NAMES[b]) {
                        @Override String value() { return mOverlay.isHidden(b) ? "hidden" : "shown"; }
                        @Override void step(int d) { mOverlay.setHidden(b, !mOverlay.isHidden(b)); }
                    });
                }
                return l;
            }
        };
    }

    /** Settings: Saves, Export log, Boot animation, Graphics driver, About and licences. */
    private SideMenu.Page settingsPage() {
        return new SideMenu.Page() {
            public String title() { return "Settings"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                l.add(new SideMenu.Item("Saves") { @Override void run() { mMenu.push(savesPage()); } });
                l.add(new SideMenu.Item("Export log") { @Override void run() { mMenu.push(logPage()); } });
                l.add(new SideMenu.Item("Boot animation: " + (bootVideo(ZoeActivity.this).exists() ? "boot.mpg" : "none")) {
                    @Override void run() { mMenu.push(bootPage()); }
                });
                String drv = driverName(ZoeActivity.this);
                l.add(new SideMenu.Item("Graphics driver: " + (drv != null ? drv : "system")) {
                    @Override void run() { mMenu.push(driverPage()); }
                });
                l.add(new SideMenu.Item("About and licences") { @Override void run() { mMenu.push(aboutPage()); } });
                return l;
            }
        };
    }

    /** About and licences: assets/licenses/README.txt line by line, when the build has one. */
    private SideMenu.Page aboutPage() {
        return new SideMenu.Page() {
            public String title() { return "About and licences"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(
                         getAssets().open("licenses/README.txt"), java.nio.charset.StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = r.readLine()) != null) if (!line.trim().isEmpty()) l.add(new SideMenu.Item(line.trim()));
                } catch (IOException e) {
                    l.add(new SideMenu.Item("(no licence list in this build)"));
                }
                return l;
            }
        };
    }

    private void message(String title, String text) {
        new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(title).setMessage(text).setPositiveButton("OK", null).show();
    }

    private static final int PICK_DRIVER = 4711, PICK_BOOT = 4712, PICK_SAVE_EXPORT = 4713, PICK_SAVE_IMPORT = 4714,
                             PICK_LOG_EXPORT = 4715;
    private static final android.net.Uri DOWNLOADS =
        android.net.Uri.parse("content://com.android.providers.downloads.documents/root/downloads");

    private void pickOpen(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI, DOWNLOADS);
        startActivityForResult(i, request);
    }

    private void pickCreate(int request, String mime, String name) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(mime);
        i.putExtra(Intent.EXTRA_TITLE, name);
        i.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI, DOWNLOADS);
        startActivityForResult(i, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request < PICK_DRIVER || request > PICK_LOG_EXPORT) {     // SDL's own file dialog
            super.onActivityResult(request, result, data);
            return;
        }
        immersive();
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        android.net.Uri u = data.getData();
        switch (request) {
            case PICK_BOOT: {
                String err = importBootVideo(u);
                message("Boot animation", err.isEmpty() ? "boot.mpg is stored (media/boot.mpg). The game plays it at "
                        + "the start once this port supports a boot animation." : err);
                break;
            }
            case PICK_SAVE_EXPORT: {
                String err = exportSaves(u);
                message("Saves", err.isEmpty() ? "Your memory card is exported. Keep the zip file somewhere safe (it "
                        + "is not deleted when the app is uninstalled)." : err);
                break;
            }
            case PICK_SAVE_IMPORT: {
                String err = stageSaves(u);
                if (!err.isEmpty()) { message("Saves", err); break; }
                new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Saves")
                    .setMessage("The saves are ready. The game restarts now to use them; your current memory card is "
                                + "kept as a backup in memcard_backup/.")
                    .setCancelable(false)
                    .setPositiveButton("Restart now", (dlg, w) -> { restartApp(); System.exit(0); })
                    .setNegativeButton("Cancel", (dlg, w) -> deleteTree(new File(userDir(this), SAVES_STAGE)))
                    .show();
                break;
            }
            case PICK_LOG_EXPORT: {
                String err = exportLog(u);
                message("Export log", err.isEmpty() ? "The log is exported. Please attach this file to your report." : err);
                break;
            }
            case PICK_DRIVER: {
                final String[] slot = { null };
                String err = importDriver(u, slot);
                if (!err.isEmpty()) { message("Graphics driver", err); break; }
                new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Graphics driver")
                    .setMessage(keptDriverName(new File(driversDir(this), slot[0])) + " is added. Use it now? The game "
                                + "restarts to switch the driver.")
                    .setPositiveButton("Use now", (dlg, w) -> switchToKept(slot[0]))
                    .setNegativeButton("Later", null)
                    .show();
                break;
            }
            default: break;
        }
    }

    // ---- Storage --------------------------------------------------------------------------------------------

    /** The user's files (SDL's external storage path): /storage/emulated/0/Android/data/<package>/files - memcard/,
     *  pc_settings.ini, data/logs/, media/; reachable with adb and file managers. Internal storage when none is
     *  mounted. */
    static File userDir(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        return ext != null && android.os.Environment.MEDIA_MOUNTED.equals(android.os.Environment.getExternalStorageState(ext))
            ? ext : ctx.getFilesDir();
    }

    /** memcard/ and media/ made here, before the game, group-writable (adb push / file managers); shareTree on every
     *  start also fixes what the game created since. */
    private void prepareUserDirs() {
        for (String name : new String[] { "memcard", "media" }) {
            File d = new File(userDir(this), name);
            d.mkdirs();
            shareTree(d);
        }
    }

    /** Dirs 2770, files 0660 (the app writes -rw------- by default). */
    private static void shareTree(File f) {
        try {
            android.system.Os.chmod(f.getPath(), f.isDirectory() ? 02770 : 0660);
        } catch (Exception ignored) { }
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null) for (File k : kids) shareTree(k);
    }

    private static int copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            to.mkdirs();
            int n = 0;
            File[] kids = from.listFiles();
            if (kids != null) for (File k : kids) n += copyTree(k, new File(to, k.getName()));
            return n;
        }
        java.nio.file.Files.copy(from.toPath(), to.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        return 1;
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private static String readText(File f) {
        try { return new String(java.nio.file.Files.readAllBytes(f.toPath())).trim(); } catch (IOException e) { return ""; }
    }

    private static String stamp() {
        return new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.ROOT).format(new java.util.Date());
    }

    private static int countFiles(File f) {
        if (!f.isDirectory()) return f.isFile() ? 1 : 0;
        int n = 0;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) n += countFiles(k);
        return n;
    }

    // ---- Saves export / import (Orta 2026-11-13: uninstalling deletes Android/data/<package>/files, so the memory
    // card needs a way out). Export = one zip of memcard/ (entries "memcard/..."), written through the system file
    // picker. Import = the zip unpacked next to memcard/ (SAVES_STAGE), then a restart; applyPendingSaves swaps it in
    // at the next start, BEFORE the game runs (the running game keeps its memory card open / cached). The memory card
    // replaced by an import is zipped to memcard_backup/ first (the newest 5 kept).
    private static final String SAVES_STAGE = ".memcard_import";

    private SideMenu.Page savesPage() {
        return new SideMenu.Page() {
            public String title() { return "Saves"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                l.add(new SideMenu.Item("Import saves") { @Override void run() { mMenu.close(); pickOpen(PICK_SAVE_IMPORT); } });
                l.add(new SideMenu.Item("Export save") {
                    @Override void run() { mMenu.close(); pickCreate(PICK_SAVE_EXPORT, "application/zip", "ZOE-memcard-" + stamp() + ".zip"); }
                });
                l.add(new SideMenu.Item("(uninstalling the app deletes its saves - export them first)"));
                l.add(new SideMenu.Item("(an import restarts the game; the old memory card goes to memcard_backup/)"));
                return l;
            }
        };
    }

    /** memcard/ as a zip (entries "memcard/<path>") into out; returns the file count. */
    private static int zipSaves(File memcard, OutputStream out) throws IOException {
        int[] n = { 0 };
        try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(out)) {
            zipAdd(z, memcard, "memcard", n);
        }
        return n[0];
    }

    private static void zipAdd(java.util.zip.ZipOutputStream z, File f, String name, int[] n) throws IOException {
        if (f.isDirectory()) {
            z.putNextEntry(new java.util.zip.ZipEntry(name + "/"));
            z.closeEntry();
            File[] kids = f.listFiles();
            if (kids != null) {
                java.util.Arrays.sort(kids);
                for (File k : kids) zipAdd(z, k, name + "/" + k.getName(), n);
            }
            return;
        }
        java.util.zip.ZipEntry e = new java.util.zip.ZipEntry(name);
        e.setTime(f.lastModified());
        z.putNextEntry(e);
        java.nio.file.Files.copy(f.toPath(), z);
        z.closeEntry();
        n[0]++;
    }

    /** Export: "" on success, else a sentence for the player. */
    private String exportSaves(android.net.Uri u) {
        File memcard = new File(userDir(this), "memcard");
        if (countFiles(memcard) == 0) return "There are no saves yet (start a game and save first).";
        try (OutputStream o = getContentResolver().openOutputStream(u, "wt")) {
            if (o == null) return "The file could not be written.";
            int n = zipSaves(memcard, o);
            Log.i(TAG, "[SAVES] exported " + n + " files -> " + u);
            return "";
        } catch (Exception e) {
            Log.e(TAG, "[SAVES] export failed: " + e);
            return "The saves could not be exported: " + e.getMessage();
        }
    }

    /** Import, step 1: the picked zip unpacked into SAVES_STAGE (checked: only relative paths, at least one save
     *  file); "" on success, else a sentence for the player. Accepts our export ("memcard/BASLUS-20148/...") and a
     *  zip of the folder's contents ("BASLUS-20148/..."). */
    private String stageSaves(android.net.Uri u) {
        File stage = new File(userDir(this), SAVES_STAGE);
        deleteTree(stage);
        stage.mkdirs();
        int n = 0;
        try (InputStream in = getContentResolver().openInputStream(u);
             java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(in)) {
            java.util.zip.ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                String name = e.getName().replace('\\', '/');
                if (name.startsWith("memcard/")) name = name.substring(8);
                if (name.isEmpty()) continue;
                if (name.startsWith("/") || name.contains(":") || ("/" + name + "/").contains("/../")) {
                    deleteTree(stage);
                    return "This zip file has paths outside its own folder - it is not a saves export.";
                }
                File f = new File(stage, name);
                if (e.isDirectory()) { f.mkdirs(); continue; }
                f.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(f)) {
                    byte[] buf = new byte[65536];
                    int r;
                    while ((r = z.read(buf)) > 0) o.write(buf, 0, r);
                }
                if (e.getTime() > 0) f.setLastModified(e.getTime());
                n++;
            }
        } catch (Exception ex) {
            deleteTree(stage);
            Log.e(TAG, "[SAVES] import failed: " + ex);
            return "The file could not be read as a zip: " + ex.getMessage();
        }
        if (n == 0) {
            deleteTree(stage);
            return "This zip file holds no saves. Please choose a file made with Export save.";
        }
        Log.i(TAG, "[SAVES] import staged: " + n + " files from " + u);
        return "";
    }

    /** Import, step 2 (onCreate, before the game): a staged import replaces memcard/; the old one is zipped to
     *  memcard_backup/ first. A failed backup leaves everything as it was (the stage stays for the next try). */
    private void applyPendingSaves() {
        File root = userDir(this), stage = new File(root, SAVES_STAGE), memcard = new File(root, "memcard");
        if (!stage.exists()) return;
        if (countFiles(stage) == 0) { deleteTree(stage); return; }
        File bdir = new File(root, "memcard_backup");
        if (memcard.isDirectory()) {
            if (countFiles(memcard) > 0) {
                bdir.mkdirs();
                File b = new File(bdir, "memcard-before-import-" + stamp() + ".zip");
                try (OutputStream o = new FileOutputStream(b)) {
                    zipSaves(memcard, o);
                } catch (IOException e) {
                    Log.e(TAG, "[SAVES] backup failed, import not applied: " + e);
                    b.delete();
                    return;
                }
                File[] old = bdir.listFiles((d, nm) -> nm.startsWith("memcard-before-import-") && nm.endsWith(".zip"));
                if (old != null && old.length > 5) {
                    java.util.Arrays.sort(old);                       // the stamp sorts by time
                    for (int i = 0; i < old.length - 5; i++) old[i].delete();
                }
            }
            deleteTree(memcard);
        }
        if (!stage.renameTo(memcard)) {
            try { copyTree(stage, memcard); } catch (IOException e) { Log.e(TAG, "[SAVES] import copy failed: " + e); return; }
            deleteTree(stage);
        }
        shareTree(memcard);
        if (bdir.exists()) shareTree(bdir);
        Log.i(TAG, "[SAVES] import applied (old memory card in " + bdir + ")");
    }

    // ---- Logs: the game's own (<ext>/data/logs/zoe_us.log, the runs before as zoe_us.1.log, zoe_us.2.log) and the
    // app's run logs (Orta 2026-11-19: every start runs `logcat --pid` of this process into
    // <files>/data/logs/run_<stamp>.txt, written as it goes, so a crash keeps everything up to it; the newest LOG_KEEP
    // runs stay). Export log writes the chosen one, or all of them, with the device / driver lines on top.
    private static final int LOG_KEEP = 5;
    private static Process sLogcat;
    private static File sRunLog;            // this start's logcat
    private File[] mLogSel;                 // what Export log writes; null = everything

    static File runLogDir(Context ctx) { return new File(ctx.getFilesDir(), "data/logs"); }
    static File gameLogDir(Context ctx) { return new File(userDir(ctx), "data/logs"); }

    private File[] runLogs() {
        File[] r = runLogDir(this).listFiles((dir, n) -> n.startsWith("run_") && n.endsWith(".txt"));
        if (r == null) return new File[0];
        java.util.Arrays.sort(r, (a, b) -> b.getName().compareTo(a.getName()));   // newest first
        return r;
    }

    private File[] gameLogs() {
        File[] r = gameLogDir(this).listFiles((dir, n) -> n.startsWith("zoe_us") && n.endsWith(".log"));
        if (r == null) return new File[0];
        java.util.Arrays.sort(r, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));   // newest first
        return r;
    }

    private static String when(File f) {
        return new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date(f.lastModified()));
    }

    private SideMenu.Page logPage() {
        return new SideMenu.Page() {
            public String title() { return "Export log"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                File[] game = gameLogs(), runs = runLogs();
                if (game.length + runs.length == 0) { l.add(new SideMenu.Item("There is no log yet.")); return l; }
                if (game.length + runs.length > 1)
                    l.add(new SideMenu.Item("Everything (" + (game.length + runs.length) + " logs)") {
                        @Override void run() { mLogSel = null; mMenu.close(); pickLogExport(); }
                    });
                for (final File g : game) {
                    l.add(new SideMenu.Item("Game log " + g.getName() + "  - " + when(g) + ", "
                                            + Math.max(1, g.length() >> 10) + " KB") {
                        @Override void run() { mLogSel = new File[] { g }; mMenu.close(); pickLogExport(); }
                    });
                }
                for (final File r : runs) {
                    String n = r.getName();         // run_yyyyMMdd-HHmmss.txt
                    String at = n.length() >= 23 ? n.substring(4, 8) + "-" + n.substring(8, 10) + "-" + n.substring(10, 12)
                        + " " + n.substring(13, 15) + ":" + n.substring(15, 17) + ":" + n.substring(17, 19) : n;
                    l.add(new SideMenu.Item("System log " + at + (r.equals(sRunLog) ? "  (this run)" : "") + "  - "
                                            + Math.max(1, r.length() >> 10) + " KB") {
                        @Override void run() { mLogSel = new File[] { r }; mMenu.close(); pickLogExport(); }
                    });
                }
                l.add(new SideMenu.Item("(the system logs of the newest " + LOG_KEEP + " starts are kept)"));
                return l;
            }
        };
    }

    private void startRunLog() {
        try {
            File d = runLogDir(this);
            d.mkdirs();
            File[] old = d.listFiles((dir, n) -> n.startsWith("run_") && n.endsWith(".txt"));
            if (old != null && old.length >= LOG_KEEP) {
                java.util.Arrays.sort(old, (a, b) -> a.getName().compareTo(b.getName()));
                for (int i = 0; i <= old.length - LOG_KEEP; i++) old[i].delete();
            }
            File f = new File(d, "run_" + stamp() + ".txt");
            sRunLog = f;
            sLogcat = new ProcessBuilder("logcat", "-v", "time", "--pid=" + android.os.Process.myPid())
                .redirectErrorStream(true).redirectOutput(f).start();
        } catch (Exception e) {
            Log.e(TAG, "[LOG] run log not started: " + e);
        }
    }

    private void pickLogExport() { pickCreate(PICK_LOG_EXPORT, "text/plain", "zoe_log_" + stamp() + ".txt"); }

    /** Export: "" on success, else a sentence for the player. */
    private String exportLog(android.net.Uri u) {
        File[] logs = mLogSel;
        if (logs == null) {
            File[] game = gameLogs(), runs = runLogs();
            logs = new File[game.length + runs.length];
            System.arraycopy(game, 0, logs, 0, game.length);
            System.arraycopy(runs, 0, logs, game.length, runs.length);
        }
        if (logs.length == 0 || !logs[0].exists()) return "There is no log yet.";
        String ver = "?";
        try { ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) { }
        String drv = driverName(this);
        try (OutputStream o = getContentResolver().openOutputStream(u, "wt")) {
            if (o == null) return "The file could not be written.";
            StringBuilder h = new StringBuilder();
            h.append("Zone of the Enders Recompiled for Android ").append(ver).append('\n')
             .append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
             .append(Build.VERSION.SDK_INT >= 31 ? " (" + Build.SOC_MODEL + ")" : "").append(", Android ").append(Build.VERSION.RELEASE)
             .append(" (API ").append(Build.VERSION.SDK_INT).append(")\n")
             .append("Driver in use: ").append(drv != null ? drv : "system").append('\n')
             .append("RAM: ").append(totalRamMb()).append(" MB; free storage: ").append(getFilesDir().getUsableSpace() >> 20).append(" MB\n");
            o.write(h.toString().getBytes());
            byte[] buf = new byte[65536];
            for (File r : logs) {
                o.write(("\n===== " + r.getName() + " (" + when(r) + ") =====\n").getBytes());
                try (InputStream in = new java.io.FileInputStream(r)) {
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
            }
            Log.i(TAG, "[LOG] exported " + logs.length + " logs -> " + u);
            return "";
        } catch (Exception e) {
            Log.e(TAG, "[LOG] export failed: " + e);
            return "The log could not be exported: " + e.getMessage();
        }
    }

    private long totalRamMb() {
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mi);
        return mi.totalMem >> 20;
    }

    // ---- Boot animation: media/boot.mpg (MPEG-1 + MP2) through the system picker; stored only - the game side reads
    // it later.

    static File bootVideo(Context ctx) { return new File(userDir(ctx), "media/boot.mpg"); }

    private SideMenu.Page bootPage() {
        return new SideMenu.Page() {
            public String title() { return "Boot animation"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                final File f = bootVideo(ZoeActivity.this);
                l.add(new SideMenu.Item("None" + (f.exists() ? "" : "  (in use)")) {
                    @Override void run() { if (f.exists()) { f.delete(); mMenu.refresh(); } }
                });
                l.add(new SideMenu.Item("Import file..." + (f.exists() ? "  (in use: boot.mpg, " + (f.length() >> 20) + " MB)" : "")) {
                    @Override void run() { mMenu.close(); pickOpen(PICK_BOOT); }
                });
                return l;
            }
        };
    }

    /** The picked file to media/boot.mpg (an MPEG-1 system stream starts with the pack header 00 00 01 BA); "" on
     *  success, else a sentence for the player. */
    private String importBootVideo(android.net.Uri u) {
        File out = bootVideo(this), tmp = new File(out.getPath() + ".new");
        out.getParentFile().mkdirs();
        long n = 0;
        try (InputStream in = getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(tmp)) {
            if (in == null) return "The file could not be read.";
            byte[] buf = new byte[1 << 20];
            int r;
            boolean first = true;
            while ((r = in.read(buf)) > 0) {
                if (first && (r < 4 || buf[0] != 0 || buf[1] != 0 || buf[2] != 1 || (buf[3] & 0xFF) != 0xBA)) {
                    tmp.delete();
                    return "This is not an MPEG-1 video (boot.mpg). Convert your video to MPEG-1 + MP2 first (see the README).";
                }
                first = false;
                o.write(buf, 0, r);
                n += r;
            }
        } catch (Exception e) {
            tmp.delete();
            return "The file could not be copied: " + e.getMessage();
        }
        if (out.exists()) out.delete();
        if (!tmp.renameTo(out)) { tmp.delete(); return "The file could not be stored."; }
        shareTree(out);
        Log.i(TAG, "[MEDIA] boot.mpg imported: " + n + " bytes");
        return "";
    }

    // ---- Graphics driver: the system driver or a Turnip package the player added (.adpkg.zip, e.g. purple-turnip).
    // Up to MAX_DRIVERS packages are kept, each unpacked in its own folder <files>/drivers/<n>/ (the .so + meta.json).
    // The one in use is a copy in <files>/turnip/ with driver.txt (line 1 the library's file name - what
    // vkshim/vulkan_zoe.c reads -, line 2 the display name, line 3 the kept folder it came from). The Vulkan library
    // picks its driver once per process, so every switch restarts the game (RestartActivity).
    private static final int MAX_DRIVERS = 5;

    static File driversDir(Context ctx) { return new File(ctx.getFilesDir(), "drivers"); }
    static File turnipDir(Context ctx) { return new File(ctx.getFilesDir(), "turnip"); }

    /** The kept driver folders, by number. */
    private File[] keptDrivers() {
        File[] d = driversDir(this).listFiles(f -> f.isDirectory() && f.getName().matches("[0-9]+")
                                                  && new File(f, "meta.json").isFile());
        if (d == null) return new File[0];
        java.util.Arrays.sort(d, (a, b) -> Integer.compare(Integer.parseInt(a.getName()), Integer.parseInt(b.getName())));
        return d;
    }

    /** meta.json "name" (+ "driverVersion"), else null. */
    private static String metaName(File dir) {
        try {
            org.json.JSONObject j = new org.json.JSONObject(readText(new File(dir, "meta.json")));
            String n = j.optString("name", "").trim();
            if (n.isEmpty()) return null;
            String v = j.optString("driverVersion", "").trim();
            return v.isEmpty() ? n : n + " " + v;
        } catch (Exception e) { return null; }
    }

    private static String metaLib(File dir) {
        try {
            String lib = new org.json.JSONObject(readText(new File(dir, "meta.json"))).optString("libraryName", "");
            return lib.isEmpty() ? null : new File(lib).getName();
        } catch (Exception e) { return null; }
    }

    private static String keptDriverName(File dir) {
        String n = metaName(dir);
        if (n != null) return n;
        String lib = metaLib(dir);
        return lib != null ? lib : "Driver " + dir.getName();
    }

    /** The driver in use: null = the system driver; else meta.json's name in turnip/, else driver.txt's library. */
    static String driverName(Context ctx) {
        File dir = turnipDir(ctx);
        String[] l = readText(new File(dir, "driver.txt")).split("\n");
        if (l[0].trim().isEmpty() || !new File(dir, l[0].trim()).isFile()) return null;
        String n = metaName(dir);
        return n != null ? n : l[0].trim();
    }

    /** The kept folder the driver in use came from (driver.txt line 3), "" if none. */
    private String driverSlotInUse() {
        if (driverName(this) == null) return "";
        String[] l = readText(new File(turnipDir(this), "driver.txt")).split("\n");
        return l.length > 2 ? l[2].trim() : "";
    }

    private SideMenu.Page driverPage() {
        return new SideMenu.Page() {
            public String title() { return "Graphics driver"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                final String drv = driverName(ZoeActivity.this), inUse = driverSlotInUse();
                l.add(new SideMenu.Item("In use: " + (drv != null ? drv : "System driver")));
                File[] kept = keptDrivers();
                for (final File k : kept) {
                    final boolean cur = k.getName().equals(inUse);
                    l.add(new SideMenu.Item(keptDriverName(k) + (cur ? "  (in use)" : "")) {
                        @Override void run() { if (!cur) switchToKept(k.getName()); }
                    });
                }
                if (kept.length < MAX_DRIVERS)
                    l.add(new SideMenu.Item("Add driver (.adpkg.zip)...") {
                        @Override void run() { mMenu.close(); pickOpen(PICK_DRIVER); }
                    });
                else
                    l.add(new SideMenu.Item("(" + MAX_DRIVERS + " drivers kept - remove one to add another)"));
                if (kept.length > 0)
                    l.add(new SideMenu.Item("Remove a kept driver...") { @Override void run() { mMenu.push(removeDriverPage()); } });
                l.add(new SideMenu.Item("Use system driver" + (drv == null ? "  (in use)" : "")) {
                    @Override void run() {
                        if (drv == null) return;
                        new File(turnipDir(ZoeActivity.this), "driver.txt").delete();
                        Log.i(TAG, "[DRIVER] system driver chosen");
                        restartApp();
                        System.exit(0);
                    }
                });
                l.add(new SideMenu.Item("(the game restarts to switch the driver)"));
                return l;
            }
        };
    }

    private SideMenu.Page removeDriverPage() {
        return new SideMenu.Page() {
            public String title() { return "Remove a kept driver"; }
            public java.util.List<SideMenu.Item> items() {
                java.util.List<SideMenu.Item> l = new java.util.ArrayList<>();
                final String inUse = driverSlotInUse();
                for (final File k : keptDrivers()) {
                    final String name = keptDriverName(k);
                    final boolean cur = k.getName().equals(inUse);
                    l.add(new SideMenu.Item("Remove " + name + (cur ? "  (in use)" : "")) {
                        @Override void run() {
                            new android.app.AlertDialog.Builder(ZoeActivity.this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                                .setTitle("Graphics driver")
                                .setMessage("Remove " + name + " from the kept drivers?" + (cur ? " It stays in use until "
                                            + "you choose another driver." : ""))
                                .setPositiveButton("Remove", (dlg, w) -> {
                                    deleteTree(k);
                                    if (cur) {          // the copy in turnip/ stays in use, no longer tied to a kept folder
                                        File txt = new File(turnipDir(ZoeActivity.this), "driver.txt");
                                        String[] t = readText(txt).split("\n");
                                        try (OutputStream o = new FileOutputStream(txt)) {
                                            o.write((t[0].trim() + "\n" + (t.length > 1 ? t[1].trim() : "") + "\n").getBytes());
                                        } catch (IOException ignored) { }
                                    }
                                    Log.i(TAG, "[DRIVER] removed kept driver " + k.getName() + " (" + name + ")");
                                    mMenu.back();
                                })
                                .setNegativeButton("Cancel", null)
                                .show();
                        }
                    });
                }
                if (l.isEmpty()) l.add(new SideMenu.Item("No driver kept"));
                return l;
            }
        };
    }

    /** A kept driver into turnip/ (a copy: removing the kept one later never breaks the one in use), then restart. */
    private void switchToKept(String slot) {
        File src = new File(driversDir(this), slot), dst = turnipDir(this);
        String lib = metaLib(src);
        if (lib == null || !new File(src, lib).isFile()) { message("Graphics driver", "This driver package is incomplete. Please remove it and add it again."); return; }
        File tmp = new File(getFilesDir(), "data/tmp/turnip_new");
        deleteTree(tmp);
        try {
            copyTree(src, tmp);
            try (OutputStream o = new FileOutputStream(new File(tmp, "driver.txt"))) {
                o.write((lib + "\n" + keptDriverName(src) + "\n" + slot + "\n").getBytes());
            }
        } catch (IOException e) {
            deleteTree(tmp);
            message("Graphics driver", "The driver could not be installed (storage): " + e.getMessage());
            return;
        }
        deleteTree(dst);
        if (!tmp.renameTo(dst)) { deleteTree(tmp); message("Graphics driver", "The driver could not be installed (storage)."); return; }
        Log.i(TAG, "[DRIVER] chosen: " + keptDriverName(src) + " (" + lib + ", kept " + slot + ")");
        restartApp();
        System.exit(0);
    }

    /** Turnip drivers run only on Adreno GPUs (Qualcomm SoCs); on any other GPU the game would end at its first
     *  Vulkan call. Android 12+ names the SoC maker; older: allowed. */
    private static boolean maybeAdreno() {
        if (Build.VERSION.SDK_INT < 31) return true;
        String m = Build.SOC_MANUFACTURER;
        if (m == null || m.isEmpty() || m.equalsIgnoreCase("unknown")) return true;
        m = m.toLowerCase(java.util.Locale.ROOT);
        return m.contains("qti") || m.contains("qualcomm");
    }

    /** An adrenotools .adpkg.zip (meta.json + the driver .so) the player picked, unpacked into the lowest free
     *  drivers/<n>/. Returns "" on success (slot[0] = the folder's name), else a sentence for the player. */
    private String importDriver(android.net.Uri uri, String[] slot) {
        if (!maybeAdreno())
            return "Turnip drivers work only on Adreno GPUs (Snapdragon). This device has a " + Build.SOC_MANUFACTURER
                   + " " + Build.SOC_MODEL + ". Please keep the system driver.";
        File[] kept = keptDrivers();
        if (kept.length >= MAX_DRIVERS) return MAX_DRIVERS + " drivers are kept already - please remove one first.";
        File tmp = new File(getFilesDir(), "data/tmp/driver_new");
        deleteTree(tmp);
        tmp.mkdirs();
        String lib;
        try (InputStream in = getContentResolver().openInputStream(uri);
             java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(in)) {
            java.util.zip.ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                String n = new File(e.getName()).getName();      // flat: no folders, no ".."
                if (e.isDirectory() || !(n.equals("meta.json") || n.endsWith(".so"))) continue;
                try (OutputStream o = new FileOutputStream(new File(tmp, n))) {
                    byte[] buf = new byte[65536];
                    int r;
                    while ((r = z.read(buf)) > 0) o.write(buf, 0, r);
                }
            }
            if (!new File(tmp, "meta.json").exists()) {
                deleteTree(tmp);
                return "This file is not a driver package (no meta.json). Please choose a Turnip .adpkg.zip.";
            }
            lib = metaLib(tmp);
            if (lib == null || !new File(tmp, lib).exists()) {
                deleteTree(tmp);
                return "The driver package names a library it does not contain.";
            }
        } catch (Exception ex) {
            deleteTree(tmp);
            Log.e(TAG, "[DRIVER] cannot import " + uri + ": " + ex);
            return "The driver package could not be read: " + ex.getMessage();
        }
        // the same package twice (same name, same library size): not kept again
        String name = keptDriverName(tmp);
        long size = new File(tmp, lib).length();
        for (File k : kept) {
            String kl = metaLib(k);
            if (name.equals(keptDriverName(k)) && kl != null && new File(k, kl).length() == size) {
                deleteTree(tmp);
                return name + " is kept already.";
            }
        }
        java.util.Set<String> used = new java.util.HashSet<>();
        for (File k : kept) used.add(k.getName());
        File[] all = driversDir(this).listFiles();                   // also half-made folders (no meta.json)
        if (all != null) for (File a : all) used.add(a.getName());
        int n = 1;
        while (used.contains(String.valueOf(n))) n++;
        File dst = new File(driversDir(this), String.valueOf(n));
        driversDir(this).mkdirs();
        if (!tmp.renameTo(dst)) { deleteTree(tmp); return "The driver could not be stored (storage)."; }
        slot[0] = dst.getName();
        Log.i(TAG, "[DRIVER] kept " + name + " (" + lib + ") as drivers/" + slot[0]);
        return "";
    }

    /** The driver switch and the saves import: an app cannot exec itself, and a PendingIntent alarm is a blocked
     *  background start on Android 14+ (Orta, S23 2026-11-13) - so RestartActivity (its own process) is started now,
     *  while this one is in the foreground; the caller ends this process right after, RestartActivity waits for that
     *  and starts the game again. */
    public void restartApp() {
        Intent i = new Intent(this, RestartActivity.class);
        i.putExtra(RestartActivity.EXTRA_PID, android.os.Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }
}
