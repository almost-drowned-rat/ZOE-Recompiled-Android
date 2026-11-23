// SetupRunner - the first-start build ON THE DEVICE (agent L, 2026-11-23): the PC launcher's steps (engine/launcher
// launcher.c) for the Android app, run by SetupActivity on a thread of its own. The APK holds no game code: the
// game is translated from the player's own disc and compiled here.
//
//   Preparing the tools      assets/setup/{python,tools,toolchain}.zip -> <files>/setup/ (make_setup_assets.py)
//   Copying the disc image   the picked document -> <files>/setup/disc.iso (a child process cannot open the
//                            picker's document by path; the copy is deleted when the disc is unpacked)
//   Checking the disc image  lift.txt "check" (exit 3: not the game's disc)
//   Translating              lift.txt's other lines (python = libpython_exe.so)
//   Compiling, Linking       build.txt (clang = libclang_exe.so, lld through libld_exe.so) -> libZOE_game.so
//   Graphics driver          the player's Turnip .adpkg.zip kept + made the one in use (the BACK menu's slots)
//   Unpacking the disc       libzoe_run_exe.so = the built game: --unpack-disc / --unpack-extra (as the PC)
//   Copying the subtitles    -> <external files>/game/ZOE-subtitles.pak
//   done: <files>/bin/libZOE_game.so + bin/setup.txt (the kit's id); <files>/setup/ removed.
//
// Cancel: every running program destroyed, the half-made files removed - the game counts as built only when
// bin/setup.txt names the APK's kit.
// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.almostdrownedrat.zoe;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class SetupRunner implements Runnable {
    private static final String TAG = "ZOE-SETUP";
    static final int WRONG_DISC = 3;
    /** Where a failed setup's log is (the game's BACK menu does not exist yet). */
    static final String LOG_HINT = ".\n\nThe log: Android/data/io.github.almostdrownedrat.zoe/files/data/logs/setup.log"
            + " - a file manager that shows Android/data opens it (or adb). Please attach it to a bug report.";

    interface Listener {
        void step(String text, float fraction);     // the whole setup's fraction 0..1
        void done();
        void failed(String message, boolean wrongDisc);
    }

    /** What the player chose (null = the optional line empty). */
    static final class Choice { Uri disc, discJa, subtitles, driver; }

    private final Context ctx;
    private final Choice choice;
    private final Listener out;
    private final File files, ext, S, native_;
    private volatile boolean cancelled;
    private final List<Process> running = Collections.synchronizedList(new ArrayList<>());
    private ExecutorService pool;

    // the steps' shares of the bar (rough times on an Odin 2: tools 10 s, disc 30 s, translate 2 min, compile 8 min)
    private float base, span;

    SetupRunner(Context ctx, Choice choice, Listener out) {
        this.ctx = ctx.getApplicationContext();
        this.choice = choice;
        this.out = out;
        files = ctx.getFilesDir();
        ext = ctx.getExternalFilesDir(null);
        S = new File(files, "setup");
        native_ = new File(ctx.getApplicationInfo().nativeLibraryDir);
    }

    // ---- what the app asks before it starts the game ----------------------------------------------------------

    static String kitVersion(Context c) {
        try (InputStream in = c.getAssets().open("setup/version.txt")) {
            return new String(readAll(in), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    static File gameLibrary(Context c) { return new File(c.getFilesDir(), "bin/libZOE_game.so"); }

    /** The game was built by this APK's kit (else: the setup first - a new APK brings a new engine). */
    static boolean isInstalled(Context c) {
        String want = kitVersion(c);
        if (want.isEmpty() || !gameLibrary(c).isFile()) return false;
        try (InputStream in = new FileInputStream(new File(c.getFilesDir(), "bin/setup.txt"))) {
            return new String(readAll(in), StandardCharsets.UTF_8).trim().equals(want);
        } catch (IOException e) {
            return false;
        }
    }

    // ---- the run ---------------------------------------------------------------------------------------------------

    void cancel() {
        cancelled = true;
        synchronized (running) {
            for (Process p : running) p.destroyForcibly();
        }
        if (pool != null) pool.shutdownNow();
    }

    private static final class Failed extends Exception {
        final boolean wrongDisc;
        Failed(String m) { this(m, false); }
        Failed(String m, boolean w) { super(m); wrongDisc = w; }
    }
    private static final class Cancelled extends Exception { }

    @Override
    public void run() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
        try {
            work();
            if (!cancelled) out.done();
        } catch (Cancelled c) {
            Log.i(TAG, "cancelled");
        } catch (Failed f) {
            Log.e(TAG, "failed: " + f.getMessage());
            if (!cancelled) out.failed(f.getMessage(), f.wrongDisc);
        } catch (Throwable t) {
            Log.e(TAG, "failed", t);
            if (!cancelled) out.failed("The setup stopped: " + t, false);
        } finally {
            if (cancelled) cleanUp(false);
            else leftovers();
        }
    }

    /** After a failure: the big files gone (the disc copies, the build), the unpacked tools kept for a retry. */
    private void leftovers() {
        if (!S.isDirectory()) return;
        new File(S, "disc.iso").delete();
        new File(S, "disc_ja.iso").delete();
        new File(S, "disc.iso.part").delete();
        new File(S, "disc_ja.iso.part").delete();
        new File(S, "libZOE_game.so").delete();
        File log = new File(S, "setup.log");
        // the failed compilers' messages into the setup's log (the per-file logs go with the build)
        File[] logs = new File(S, "work/obj").listFiles(f -> f.getName().endsWith(".log")
                                                             && !readText(f).trim().endsWith("(exit 0)"));
        if (logs != null)
            for (File l : logs) {
                String t = readText(l);
                appendLog(log, "\n--- " + l.getName() + "\n" + (t.length() > 8000 ? t.substring(0, 8000) + "\n..." : t));
            }
        deleteTree(new File(S, "work"));
        if (log.isFile()) copyLog(log);
    }

    private void check() throws Cancelled { if (cancelled) throw new Cancelled(); }

    private void phase(String text, float b, float s, float within) {
        base = b;
        span = s;
        say(text, within);
    }
    private String stepText = "";
    private void say(String text, float within) {
        stepText = text;
        out.step(text, base + span * Math.max(0, Math.min(1, within)));
    }

    private void work() throws Exception {
        boolean ja = choice.discJa != null, subs = choice.subtitles != null, drv = choice.driver != null;
        // the bar: tools 2 %, disc copy 5 %, check 1 %, translate 16 %, compile 64 %, link 1 %, driver 1 %, unpack 6 %
        // (+ the Japanese disc 4 %); the rest scaled to 1
        float tTools = 2, tCopy = 5, tCheck = 1, tLift = 16, tCc = 64, tLink = 1, tDrv = drv ? 1 : 0, tUnpack = 6,
              tJa = ja ? 4 : 0, tSubs = subs ? 0.2f : 0;
        float sum = tTools + tCopy + tCheck + tLift + tCc + tLink + tDrv + tUnpack + tJa + tSubs, at = 0;

        File log = new File(S, "setup.log");
        // room: the disc image's copy, the build (~1.5 GB at its largest) and the unpacked disc
        long need = size(choice.disc) * 2 + (1500L << 20);
        if (files.getUsableSpace() < need)
            throw new Failed("Not enough free storage: the setup needs about " + (need >> 30) + " GB free ("
                             + (files.getUsableSpace() >> 20) + " MB free now).");

        // 1. the tools
        phase("Preparing the tools", at / sum, tTools / sum, 0);
        String kit = kitVersion(ctx);
        File kitStamp = new File(S, "kit.txt");
        if (!kit.equals(readText(kitStamp))) {
            deleteTree(S);
            S.mkdirs();
            String[] zips = { "python", "tools", "toolchain" };
            for (int i = 0; i < zips.length; i++) {
                final int k = i;
                unzipAsset("setup/" + zips[i] + ".zip", new File(S, zips[i].equals("toolchain") ? "tc" : zips[i]),
                           f -> say("Preparing the tools", (k + f) / zips.length));
            }
            writeText(kitStamp, kit);
        }
        S.mkdirs();
        new File(S, "tmp").mkdirs();
        at += tTools;

        // 2. the disc image
        phase("Copying the disc image", at / sum, tCopy / sum, 0);
        File iso = new File(S, "disc.iso");
        copyUri(choice.disc, iso, f -> say("Copying the disc image", f));
        at += tCopy;

        // 3. check + 4. translate (lift.txt: name, python's arguments)
        File tools = new File(S, "tools"), work = new File(S, "work");
        List<String[]> lift = new ArrayList<>();
        for (String line : readText(new File(tools, "lift.txt")).split("\n")) {
            String[] a = line.split("\t");
            if (a.length >= 2 && !a[0].startsWith("#")) lift.add(a);
        }
        int jobs = jobs();
        phase("Checking the disc image", at / sum, tCheck / sum, 0);
        for (String[] a : lift) {
            if (!a[0].equals("check")) continue;
            int rc = python(a, iso, work, jobs, log);
            if (rc == WRONG_DISC)
                throw new Failed("That image is not the Zone of the Enders (USA) disc (SLUS-20148) - or another version of "
                                 + "it. Please choose your US disc image.", true);
            if (rc != 0) throw new Failed("Checking the disc image failed" + LOG_HINT);
        }
        at += tCheck;
        phase("Translating the game's code", at / sum, tLift / sum, 0);
        deleteTree(work);
        work.mkdirs();
        long t0 = System.currentTimeMillis();
        for (String[] a : lift) {
            if (a[0].equals("check")) continue;
            final long started = t0;
            int rc = python(a, iso, work, jobs, log,
                            () -> say("Translating the game's code", (System.currentTimeMillis() - started) / 150000f * 0.95f));
            if (rc != 0) throw new Failed("Translating the game's code failed" + LOG_HINT);
        }
        at += tLift;

        // 5. compile + 6. link (build.txt)
        phase("Compiling", at / sum, tCc / sum, 0);
        File game = new File(S, "libZOE_game.so");
        compileAndLink(tools, work, game, jobs, log, at / sum, tCc / sum, (at + tCc) / sum, tLink / sum);
        at += tCc + tLink;

        // 7. the graphics driver
        if (drv) {
            phase("Installing the graphics driver", at / sum, tDrv / sum, 0);
            String[] slot = new String[1];
            String err = ZoeActivity.importDriver(ctx, choice.driver, slot);
            final String KEPT = " is kept already.";
            if (err.endsWith(KEPT)) {                                // the same package: the kept one is used
                String name = err.substring(0, err.length() - KEPT.length());
                for (File k : ZoeActivity.keptDrivers(ctx))
                    if (ZoeActivity.keptDriverName(k).equals(name)) slot[0] = k.getName();
                if (slot[0] != null) err = "";
            }
            if (!err.isEmpty()) throw new Failed(err);
            if (slot[0] != null) {
                err = ZoeActivity.activateKept(ctx, slot[0]);
                if (!err.isEmpty()) throw new Failed(err);
            }
            at += tDrv;
        }
        check();

        // 8. the disc unpacked into the game's folder (the game reads it from there)
        File gameDir = new File(ext, "game"), discDir = new File(gameDir, "disc");
        gameDir.mkdirs();
        phase("Unpacking the disc into the game folder", at / sum, tUnpack / sum, 0);
        unpack(game, "--unpack-disc", iso, discDir, null, log, "Unpacking the disc into the game folder");
        iso.delete();
        at += tUnpack;

        // 9. the Japanese disc (optional): only what the port reads of it
        if (ja) {
            phase("Copying the Japanese disc", at / sum, tJa / sum, 0);
            File isoJa = new File(S, "disc_ja.iso");
            copyUri(choice.discJa, isoJa, f -> say("Copying the Japanese disc", f * 0.5f));
            say("Unpacking the second disc", 0.5f);
            unpack(game, "--unpack-extra", isoJa, new File(gameDir, "disc_ja"), discDir, log, "Unpacking the second disc");
            isoJa.delete();
            at += tJa;
        }

        // 10. the subtitle pack (optional)
        if (subs) {
            phase("Copying the subtitles", at / sum, tSubs / sum, 0);
            copyUri(choice.subtitles, new File(gameDir, "ZOE-subtitles.pak"), f -> say("Copying the subtitles", f));
        }
        check();

        // done: the game where ZoeActivity loads it, the stamp last
        File bin = new File(files, "bin");
        bin.mkdirs();
        File dst = gameLibrary(ctx);
        dst.delete();
        if (!game.renameTo(dst)) throw new Failed("The game could not be stored (storage).");
        dst.setReadable(true, true);
        dst.setWritable(true, true);
        dst.setExecutable(false, false);
        writeText(new File(bin, "setup.txt"), kit);
        copyLog(log);
        out.step("Done", 1f);
        cleanUp(true);
    }

    /** Compilers at once: the cores, at most one per 1.4 GB of memory (the largest translated files take ~1 GB). */
    private int jobs() {
        android.app.ActivityManager am = (android.app.ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        int n = Runtime.getRuntime().availableProcessors();
        int byMem = (int) (mi.totalMem / (1400L << 20));
        return Math.max(1, Math.min(n, Math.max(1, byMem)));
    }

    // ---- the programs ----------------------------------------------------------------------------------------------

    private String expand(String s, File iso, File work, int jobs, File out) {
        return s.replace("{TOOLS}", new File(S, "tools").getPath()).replace("{TC}", new File(S, "tc").getPath())
                .replace("{WORK}", work.getPath()).replace("{ISO}", iso == null ? "" : iso.getPath())
                .replace("{JOBS}", String.valueOf(jobs)).replace("{NATIVE}", native_.getPath())
                .replace("{OUT}", out == null ? "" : out.getPath()).replace("{ROOT}", files.getPath());
    }

    private int python(String[] line, File iso, File work, int jobs, File log) throws Exception {
        return python(line, iso, work, jobs, log, null);
    }

    private int python(String[] line, File iso, File work, int jobs, File log, Runnable tick) throws Exception {
        List<String> cmd = new ArrayList<>();
        cmd.add(new File(native_, "libpython_exe.so").getPath());
        cmd.add("-I");
        cmd.add("-B");
        for (int i = 1; i < line.length; i++) cmd.add(expand(line[i], iso, work, jobs, null));
        return exec(cmd, new File(S, "tools"), log, tick, "XR_PYTHON_HOME", new File(S, "python").getPath());
    }

    private int exec(List<String> cmd, File dir, File log, Runnable tick, String... env) throws Exception {
        check();
        ProcessBuilder pb = new ProcessBuilder(cmd).directory(dir).redirectErrorStream(true)
                .redirectOutput(ProcessBuilder.Redirect.appendTo(log));
        Map<String, String> e = pb.environment();
        e.put("LD_LIBRARY_PATH", native_.getPath());
        e.put("TMPDIR", new File(S, "tmp").getPath());
        e.put("HOME", S.getPath());
        for (int i = 0; i + 1 < env.length; i += 2) e.put(env[i], env[i + 1]);
        appendLog(log, "\n$ " + String.join(" ", cmd) + "\n");
        Process p = pb.start();
        running.add(p);
        try {
            while (true) {
                if (p.waitFor(200, java.util.concurrent.TimeUnit.MILLISECONDS)) break;
                if (tick != null) tick.run();
                if (cancelled) { p.destroyForcibly(); throw new Cancelled(); }
            }
        } finally {
            running.remove(p);
        }
        check();
        int rc = p.exitValue();
        appendLog(log, "(exit " + rc + ")\n");
        return rc;
    }

    private void compileAndLink(File tools, File work, File game, int jobs, File log, float ccBase, float ccSpan,
                                float lnBase, float lnSpan) throws Exception {
        List<String> cc = new ArrayList<>(), link = new ArrayList<>(), pre = new ArrayList<>();
        List<File> src = new ArrayList<>();
        for (String line : readText(new File(tools, "build.txt")).split("\n")) {
            String[] a = line.split("\t");
            if (a.length < 2 || a[0].startsWith("#")) continue;
            switch (a[0]) {
                case "cc": for (int i = 1; i < a.length; i++) cc.add(expand(a[i], null, work, jobs, game)); break;
                case "link": for (int i = 1; i < a.length; i++) link.add(expand(a[i], null, work, jobs, game)); break;
                case "obj": pre.add(expand(a[1], null, work, jobs, game)); break;
                case "src": src.add(new File(expand(a[1], null, work, jobs, game))); break;
                case "glob": {
                    File[] fs = new File(expand(a[1], null, work, jobs, game)).listFiles(
                            f -> f.isFile() && f.getName().endsWith(a.length > 2 ? a[2] : ".cpp"));
                    if (fs != null) Collections.addAll(src, fs);
                    break;
                }
            }
        }
        if (src.isEmpty()) throw new Failed("The translation made no source files" + LOG_HINT);
        // the largest first: the long ones start early, the short ones fill the gaps at the end
        src.sort((x, y) -> Long.compare(y.length(), x.length()));
        long total = 0;
        for (File f : src) total += f.length();
        final long all = total;
        final AtomicLong done = new AtomicLong();
        File obj = new File(work, "obj");
        obj.mkdirs();
        List<String> objects = Collections.synchronizedList(new ArrayList<>());
        pool = Executors.newFixedThreadPool(jobs);
        List<Future<Integer>> fut = new ArrayList<>();
        for (int i = 0; i < src.size(); i++) {
            final File f = src.get(i);
            final File o = new File(obj, String.format(java.util.Locale.ROOT, "%03d_%s.o", i, f.getName()));
            fut.add(pool.submit(() -> {
                List<String> cmd = new ArrayList<>();
                cmd.add(new File(native_, "libclang_exe.so").getPath());
                cmd.addAll(cc);
                cmd.add("-c");
                cmd.add(f.getPath());
                cmd.add("-o");
                cmd.add(o.getPath());
                int rc = exec(cmd, work, new File(obj, o.getName() + ".log"), null);
                if (rc == 0) {
                    objects.add(o.getPath());
                    done.addAndGet(f.length());
                }
                return rc;
            }));
        }
        pool.shutdown();
        long t0 = System.currentTimeMillis();
        while (!pool.awaitTermination(250, java.util.concurrent.TimeUnit.MILLISECONDS)) {
            check();
            say("Compiling", (float) done.get() / all);
            for (Future<Integer> x : fut)
                if (x.isDone() && !x.isCancelled() && failedJob(x)) {
                    cancelCompilers();
                    throw new Failed("Compiling failed" + LOG_HINT);
                }
        }
        check();
        for (Future<Integer> x : fut)
            if (failedJob(x)) throw new Failed("Compiling failed" + LOG_HINT);
        appendLog(log, "compiled " + src.size() + " files with " + jobs + " jobs in "
                       + (System.currentTimeMillis() - t0) / 1000 + " s\n");
        // link: {OBJECTS} = the compiled + the prebuilt objects
        base = lnBase;
        span = lnSpan;
        say("Linking", 0);
        List<String> cmd = new ArrayList<>();
        cmd.add(new File(native_, "libclang_exe.so").getPath());
        Collections.sort(objects);
        for (String a : link) {
            if (a.equals("{OBJECTS}")) { cmd.addAll(objects); cmd.addAll(pre); }
            else cmd.add(a);
        }
        game.delete();
        if (exec(cmd, work, log, null) != 0 || !game.isFile())
            throw new Failed("Linking failed" + LOG_HINT);
    }

    private static boolean failedJob(Future<Integer> x) {
        try {
            return x.get() != 0;
        } catch (Exception e) {
            return true;
        }
    }

    private void cancelCompilers() {
        if (pool != null) pool.shutdownNow();
        synchronized (running) {
            for (Process p : running) p.destroyForcibly();
        }
    }

    /** The built game as a program (libzoe_run_exe.so): --unpack-disc / --unpack-extra, progress in <to>.progress. */
    private void unpack(File game, String mode, File iso, File to, File own, File log, String text) throws Exception {
        // the old folder must go first (the engine's remove_all fails quietly): files the app may not delete - copied
        // there by a file manager or adb as another user - stop the setup with a sentence, not a bare "failed"
        deleteTree(to);
        if (to.exists())
            throw new Failed("The old folder " + to.getPath() + " cannot be replaced (its files belong to another "
                             + "app). Please delete it with a file manager or a computer, then start the setup again.");
        if (!to.getParentFile().canWrite())
            throw new Failed("The game folder " + to.getParent() + " cannot be written. Please delete it with a file "
                             + "manager or a computer, then start the setup again.");
        List<String> cmd = new ArrayList<>();
        cmd.add(new File(native_, "libzoe_run_exe.so").getPath());
        cmd.add(mode);
        cmd.add(iso.getPath());
        cmd.add(to.getPath());
        if (own != null) { cmd.add("--disc"); cmd.add(own.getPath()); }
        File prog = new File(to.getPath() + ".progress");
        prog.delete();
        float b0 = text.equals("Unpacking the second disc") ? 0.5f : 0f, s0 = 1f - b0;
        int rc = exec(cmd, S, log, () -> {
            String[] t = readText(prog).trim().split("\\s+");
            try {
                if (t.length >= 2 && Long.parseLong(t[1]) > 0)
                    say(text, b0 + s0 * (float) Long.parseLong(t[0]) / Long.parseLong(t[1]));
            } catch (NumberFormatException ignored) { }
        }, "XR_GAME_LIB", game.getPath(), "PS2HOST_HOME", ext.getPath());
        prog.delete();
        if (rc != 0) throw new Failed(text + " failed" + LOG_HINT);
    }

    // ---- files -----------------------------------------------------------------------------------------------------

    interface Frac { void at(float f); }

    private long size(Uri u) {
        if (u == null) return 0;
        try (Cursor c = ctx.getContentResolver().query(u, new String[] { OpenableColumns.SIZE }, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) { }
        return 0;
    }

    private void copyUri(Uri u, File to, Frac f) throws Exception {
        long total = size(u), n = 0;
        to.getParentFile().mkdirs();
        File tmp = new File(to.getPath() + ".part");
        try (InputStream in = ctx.getContentResolver().openInputStream(u);
             OutputStream o = new FileOutputStream(tmp)) {
            if (in == null) throw new Failed("The chosen file cannot be read - please choose it again.");
            byte[] buf = new byte[1 << 20];
            int r;
            while ((r = in.read(buf)) > 0) {
                o.write(buf, 0, r);
                n += r;
                if (total > 0) f.at((float) n / total);
                check();
            }
        } catch (SecurityException e) {
            throw new Failed("The app may no longer read the chosen file - please choose it again with Browse...");
        } finally {
            if (cancelled) tmp.delete();
        }
        to.delete();
        if (!tmp.renameTo(to)) throw new Failed("A file could not be stored (storage).");
    }

    private void unzipAsset(String asset, File dir, Frac f) throws Exception {
        long total = 1;
        try (AssetFileDescriptor fd = ctx.getAssets().openFd(asset)) { total = fd.getLength(); }
        catch (IOException ignored) { }           // (a compressed asset has no descriptor: no fraction then)
        final long all = Math.max(1, total);
        final long[] got = { 0 };
        deleteTree(dir);
        dir.mkdirs();
        String root = dir.getCanonicalPath() + File.separator;
        try (InputStream raw = ctx.getAssets().open(asset);
             ZipInputStream z = new ZipInputStream(new FilterInputStream(raw) {
                 @Override public int read(byte[] b, int off, int len) throws IOException {
                     int r = super.read(b, off, len);
                     if (r > 0) got[0] += r;
                     return r;
                 }
             })) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                check();
                File t = new File(dir, e.getName());
                if (!t.getCanonicalPath().startsWith(root)) throw new Failed("A damaged tools file (" + asset + ").");
                if (e.isDirectory()) { t.mkdirs(); continue; }
                t.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(t)) {
                    int r;
                    while ((r = z.read(buf)) > 0) o.write(buf, 0, r);
                }
                f.at(Math.min(1f, (float) got[0] / all));
            }
        }
    }

    /** The finished run's log next to the game's logs (the BACK menu's Export log finds data/logs). */
    private void copyLog(File log) {
        try {
            File d = new File(ext, "data/logs");
            d.mkdirs();
            try (InputStream in = new FileInputStream(log); OutputStream o = new FileOutputStream(new File(d, "setup.log"))) {
                byte[] buf = new byte[65536];
                int r;
                while ((r = in.read(buf)) > 0) o.write(buf, 0, r);
            }
        } catch (IOException ignored) { }
    }

    /** ok: the build's leftovers (the setup folder) removed; cancelled: also nothing half-made counts. */
    private void cleanUp(boolean ok) {
        if (!ok) {
            File log = new File(S, "setup.log");
            if (log.isFile()) copyLog(log);
        }
        deleteTree(S);
    }

    static String readText(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return new String(readAll(in), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String s) throws IOException {
        f.getParentFile().mkdirs();
        try (OutputStream o = new FileOutputStream(f)) { o.write(s.getBytes(StandardCharsets.UTF_8)); }
    }

    private static void appendLog(File log, String s) {
        try (OutputStream o = new FileOutputStream(log, true)) { o.write(s.getBytes(StandardCharsets.UTF_8)); }
        catch (IOException ignored) { }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int r;
        while ((r = in.read(buf)) > 0) b.write(buf, 0, r);
        return b.toByteArray();
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory() && !java.nio.file.Files.isSymbolicLink(f.toPath())) {
            File[] k = f.listFiles();
            if (k != null) for (File c : k) deleteTree(c);
        }
        f.delete();
    }
}
