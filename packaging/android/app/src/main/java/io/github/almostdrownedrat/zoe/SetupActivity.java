// SetupActivity - the app's first screen: the setup in the Windows Vista look (VistaSetupView), agent L 2026-11-23.
// The game is built on the device from the player's disc (SetupRunner); once built by this APK's kit, the app goes
// straight to the game (ZoeActivity). The chosen files are remembered (persisted read permission) for a rebuild.
// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.almostdrownedrat.zoe;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

public class SetupActivity extends Activity implements VistaSetupView.Host {
    private VistaSetupView mView;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (SetupRunner.isInstalled(this)) { play(); return; }
        mView = new VistaSetupView(this, this, maybeAdreno());
        setContentView(mView);
        android.content.SharedPreferences pr = getSharedPreferences("setup", MODE_PRIVATE);
        for (int row = 0; row < 3; row++) {
            String u = pr.getString("row" + row, null);
            if (u == null) continue;
            Uri uri = Uri.parse(u);
            if (!canRead(uri)) continue;
            mRows[row] = uri;
            mView.setRow(row, displayName(uri));
        }
        if (SetupRunner.gameLibrary(this).isFile())
            mView.setNote("The app was updated: the game is built again from your disc.");
        mView.requestFocus();
        if (Build.VERSION.SDK_INT >= 33)        // the back gesture (enableOnBackInvokedCallback: no onBackPressed)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, () -> mView.back());
    }

    @Override
    protected void onResume() {
        super.onResume();
        immersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (mView != null && mView.key(e)) return true;
        return super.dispatchKeyEvent(e);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() { mView.back(); }

    // ---- VistaSetupView.Host -------------------------------------------------------------------------------------

    @Override
    public void pickFile(int row) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, 100 + row);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        int row = request - 100;
        if (row < 0 || row > VistaSetupView.PICK_DRIVER) return;
        Uri u = data.getData();
        if (row < VistaSetupView.PICK_DRIVER) {
            try {
                getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) { }      // (a provider without persistable grants: this run only)
            getSharedPreferences("setup", MODE_PRIVATE).edit().putString("row" + row, u.toString()).apply();
        }
        if (row == VistaSetupView.PICK_DRIVER) mDriver = u; else mRows[row] = u;
        mView.setRow(row, displayName(u));
    }

    private boolean canRead(Uri u) {
        try (java.io.InputStream in = getContentResolver().openInputStream(u)) { return in != null; }
        catch (Exception e) { return false; }
    }

    // ---- the run ---------------------------------------------------------------------------------------------------

    private final Uri[] mRows = new Uri[3];
    private Uri mDriver;
    private SetupRunner mRunner;
    private Thread mThread;

    @Override
    public void install() {
        SetupRunner.Choice ch = new SetupRunner.Choice();
        ch.disc = mRows[0];
        ch.discJa = mRows[1];
        ch.subtitles = mRows[2];
        ch.driver = mView.driverFromFile() ? mDriver : null;
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        mRunner = new SetupRunner(this, ch, new SetupRunner.Listener() {
            @Override public void step(String text, float f) { runOnUiThread(() -> mView.progress(text, f)); }
            @Override public void done() { runOnUiThread(() -> { mView.finished(); idle(); }); }
            @Override public void failed(String m, boolean wrong) { runOnUiThread(() -> { mView.failed(m, wrong); idle(); }); }
        });
        mThread = new Thread(mRunner, "setup");
        mThread.start();
    }

    private void idle() {
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        mRunner = null;
    }

    @Override
    protected void onDestroy() {
        if (mRunner != null) mRunner.cancel();      // (the app closed during the setup: a clean stop)
        super.onDestroy();
    }

    @Override
    public void quit() {
        // during the setup ("Quit anyway?" Yes): every program stopped, the half-made files removed, then closed
        SetupRunner r = mRunner;
        Thread t = mThread;
        if (r != null) {
            r.cancel();
            new Thread(() -> {
                try { if (t != null) t.join(5000); } catch (InterruptedException ignored) { }
                runOnUiThread(this::finishAndRemoveTask);
            }).start();
            return;
        }
        finishAndRemoveTask();
    }

    @Override
    public void play() {
        Intent i = new Intent(this, ZoeActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
        finish();
    }

    // ---- helpers -------------------------------------------------------------------------------------------------

    private String displayName(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && !n.isEmpty()) return n;
            }
        } catch (Exception ignored) { }
        String s = u.getLastPathSegment();
        return s != null ? s : u.toString();
    }

    /** Turnip runs only on Adreno (Qualcomm); Android 12+ names the SoC maker, older: allowed. */
    private static boolean maybeAdreno() {
        if (Build.VERSION.SDK_INT < 31) return true;
        String m = Build.SOC_MANUFACTURER;
        if (m == null || m.isEmpty() || m.equalsIgnoreCase("unknown")) return true;
        m = m.toLowerCase(java.util.Locale.ROOT);
        return m.contains("qti") || m.contains("qualcomm");
    }

    @SuppressWarnings("deprecation")
    private void immersive() {
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
}
