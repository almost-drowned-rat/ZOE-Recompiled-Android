// SetupActivity - the app's first screen: the setup in the Windows Vista look (VistaSetupView), agent L 2026-11-23.
// PREVIEW: the installation only pretends (a timer) - nothing is read, built or installed; Play starts the game as
// before (ZoeActivity). The real first-start build (the lift + the device compile) comes after the maintainer's OK.
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
        mView = new VistaSetupView(this, this, maybeAdreno());
        setContentView(mView);
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
        startActivityForResult(i, 100 + row);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        int row = request - 100;
        if (row < 0 || row > VistaSetupView.PICK_DRIVER) return;
        mView.setRow(row, displayName(data.getData()));
    }

    @Override
    public void quit() { finishAndRemoveTask(); }

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
