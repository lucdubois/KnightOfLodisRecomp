package com.koldecomp.game;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;

/**
 * First-run setup: three boxes side by side, ROM / BIOS / Mod, each opening the
 * file picker; a note on the BIOS; Remove mod and Play.
 *
 * The ROM is required and SHA-1 checked here (the native runtime is the real
 * gate). The app may hold more than one game (Game.java): either one's ROM is
 * taken, into that game's own folder, and with both in place a button
 * switches between them. The BIOS is optional: without one, or with a file that is not the
 * GBA BIOS, the runtime's HLE BIOS (high-level emulation: built-in
 * replacements for the BIOS's functions, gbarecomp --no-bios) is used; with
 * the real one, the real one. The choice is the files/no_bios marker that
 * main_android.cpp reads, written on Play. The mod is optional (Mods.java).
 *
 * Nothing copyrighted ships in the APK: the user supplies the files, they are
 * copied into the app's private storage, and main_android.cpp reads them from
 * there via SDL_AndroidGetInternalStoragePath().
 */
public class SetupActivity extends Activity {
    private static final String TAG = "KoLSetup";

    // main_android.cpp looks for exactly these names (the ROM in its game's
    // folder, Game.rom; the BIOS in files/, every game's).
    private static final String ROM_NAME  = Game.ROM_NAME;
    private static final String BIOS_NAME = "gba_bios.bin";
    // Present = play without a BIOS; main_android.cpp then passes --no-bios.
    private static final String NO_BIOS_NAME = "no_bios";
    // Debug > Unlink ROM (unlinkRom): the ROM copy set aside, so the next
    // start is this screen; "Use the previous ROM" puts it back.
    private static final String UNLINKED_NAME = "totkol.gba.unlinked";

    // Verified in KnightOfLodisRecomp/CLAUDE.md. (The ROMs': Game.SHA1.)
    private static final String BIOS_SHA1 = "300c20df6731a33952ded8c436f7f186d25d3492";
    // Technical > Game (chooseGame): this screen stays up at the next start,
    // to take the other game's ROM or switch to it.
    private static final String CHOOSE_NAME = "choose_game";

    private static final int REQ_ROM  = 1;
    private static final int REQ_BIOS = 2;
    private static final int REQ_MOD  = 3;
    private static final int REQ_SHADERS = 4;
    // Options > Display > Shader > Import Folder (Mods.pickShaders): a folder
    // picked, copied into files/shaders/<its name>, the count to the game.
    static final String EXTRA_PICK_SHADERS = "pick_shaders";
    private boolean pickShadersOnly;

    // Started from the game (Options > Mod, Mods.java): pick a patch and go
    // back, no auto-launch.
    static final String EXTRA_PICK_MOD = "pick_mod";
    private boolean pickModOnly;
    // Options > Technical > BIOS with no BIOS file (Mods.pickBios): the picker
    // alone, the file checked, the answer to the game (Mods.nativeBiosPicked).
    static final String EXTRA_PICK_BIOS = "pick_bios";
    private boolean pickBiosOnly;

    private static final int RED = 0xFFD32F2F, GREEN = 0xFF2E7D32, GREY = 0xFF9E9E9E;

    /** One of the three boxes: its icon and the line under its name. */
    private static final class Box {
        TextView icon, line;
    }
    private Box romBox, biosBox, modBox;
    private TextView title;      // the game the boxes are for
    private TextView status;     // a copy's error, a missing ROM
    private Button removeMod, play, switchGame;
    private boolean romOk, biosOk;
    private int game = Game.USA;   // the game the boxes are for (Game.java)

    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private android.graphics.drawable.Drawable frame(int stroke) {
        android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
        g.setColor(0xFFFFFFFF);
        g.setCornerRadius(dp(12));
        g.setStroke(dp(2), stroke);
        return g;
    }

    private Box addBox(LinearLayout row, String name, Runnable onTap) {
        Box b = new Box();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setPadding(dp(12), dp(16), dp(12), dp(16));
        // Focus (the pad) and touch: the frame turns to the accent colour.
        android.graphics.drawable.StateListDrawable bg = new android.graphics.drawable.StateListDrawable();
        bg.addState(new int[] {android.R.attr.state_pressed}, frame(0xFF1976D2));
        bg.addState(new int[] {android.R.attr.state_focused}, frame(0xFF1976D2));
        bg.addState(new int[] {}, frame(0xFFBDBDBD));
        box.setBackground(bg);
        box.setFocusable(true);
        box.setClickable(true);
        box.setOnClickListener(v -> onTap.run());

        b.icon = new TextView(this);
        b.icon.setGravity(Gravity.CENTER);
        b.icon.setTextColor(0xFFFFFFFF);
        b.icon.setTextSize(22);
        b.icon.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        box.addView(b.icon, new LinearLayout.LayoutParams(dp(44), dp(44)));

        TextView label = new TextView(this);
        label.setText(name);
        label.setTextSize(18);
        label.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        label.setGravity(Gravity.CENTER);
        label.setPadding(0, dp(8), 0, dp(2));
        box.addView(label);

        b.line = new TextView(this);
        b.line.setTextSize(13);
        b.line.setGravity(Gravity.CENTER);
        b.line.setMaxLines(2);
        b.line.setEllipsize(android.text.TextUtils.TruncateAt.END);
        box.addView(b.line);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        lp.setMargins(dp(8), 0, dp(8), 0);
        row.addView(box, lp);
        boxViews.put(b, box);
        return b;
    }
    private final java.util.HashMap<Box, LinearLayout> boxViews = new java.util.HashMap<>();

    /** A box's state: the icon ("!" red, "?" grey, a green check) and its line. */
    private static void show(Box b, char icon, int color, String line) {
        android.graphics.drawable.GradientDrawable dot = new android.graphics.drawable.GradientDrawable();
        dot.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        dot.setColor(color);
        b.icon.setBackground(dot);
        b.icon.setText(String.valueOf(icon));
        b.line.setText(line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        CrashLog.install(this);   // a Java crash in files/logs/last_run.log
        // No title bar: the screen has its own title.
        if (getActionBar() != null) getActionBar().hide();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(dp(24), dp(16), dp(24), dp(16));

        // The game the boxes are for: the running one's when the game opened
        // this screen for a file, else the one that would start.
        game = Game.loaded >= 0 ? Game.loaded : Math.max(Game.current(this), Game.USA);
        Game.selected = game;

        title = new TextView(this);
        title.setText(Game.NAME[game]);
        title.setTextSize(22);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, 0, 0, dp(16));
        root.addView(title);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setBaselineAligned(false);
        romBox = addBox(row, "ROM", () -> pick(REQ_ROM));
        biosBox = addBox(row, "BIOS", () -> pick(REQ_BIOS));
        modBox = addBox(row, "Mod", () -> pick(REQ_MOD));
        root.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                        ViewGroup.LayoutParams.WRAP_CONTENT));

        // Debug > Unlink ROM left the ROM aside: put it back without a file.
        for (int g = 0; g < Game.COUNT; ++g) {
            final int which = g;
            final File aside = new File(Game.dir(this, g), UNLINKED_NAME);
            if (!aside.exists()) continue;
            Button relink = new Button(this);
            relink.setText("Use the previous ROM");
            relink.setOnClickListener(v -> {
                File rom = Game.rom(this, which);
                if (rom.exists() || aside.renameTo(rom)) {
                    root.removeView(v);
                    game = which;
                    refresh();
                }
            });
            root.addView(relink);
        }

        // Two games in place: the other one, with its own save and states.
        switchGame = new Button(this);
        switchGame.setOnClickListener(v -> {
            for (int k = 1; k < Game.COUNT; ++k) {
                final int g = (game + k) % Game.COUNT;
                if (romIs(g)) { game = g; break; }
            }
            status.setText("");
            refresh();
        });
        switchGame.setVisibility(android.view.View.GONE);
        root.addView(switchGame);

        TextView note = new TextView(this);
        note.setText("A GBA BIOS file is best: the game runs on the real BIOS, as on the console. "
                + "Without one, an HLE BIOS (built-in replacements for the BIOS's functions) "
                + "is used instead. It is less tested: some scenes may behave differently.");
        note.setTextSize(13);
        note.setGravity(Gravity.CENTER);
        note.setPadding(dp(8), dp(16), dp(8), dp(8));
        root.addView(note);

        status = new TextView(this);
        status.setTextSize(14);
        status.setTextColor(RED);
        status.setGravity(Gravity.CENTER);
        root.addView(status);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        removeMod = new Button(this);
        removeMod.setText("Remove mod");
        removeMod.setOnClickListener(v -> { Mods.clear(this); refresh(); });
        play = new Button(this);
        play.setText("Play");
        play.setOnClickListener(v -> launchGame());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        blp.setMargins(dp(8), dp(8), dp(8), 0);
        buttons.addView(removeMod, blp);
        buttons.addView(play, new LinearLayout.LayoutParams(blp));
        root.addView(buttons, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                            ViewGroup.LayoutParams.WRAP_CONTENT));

        // Scrolls: in landscape a small screen may not hold it all.
        // fillViewport keeps it centred on a tall screen.
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        refresh();
        pickModOnly = getIntent().getBooleanExtra(EXTRA_PICK_MOD, false);
        if (pickModOnly) {
            if (savedInstanceState == null) pick(REQ_MOD);
            return;
        }
        pickShadersOnly = getIntent().getBooleanExtra(EXTRA_PICK_SHADERS, false);
        if (pickShadersOnly) {
            if (savedInstanceState == null) {
                try {
                    startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_SHADERS);
                } catch (Exception e) {
                    Mods.nativeShadersImported(0, null, "This device has no folder picker.");
                    finish();
                }
            }
            return;
        }
        pickBiosOnly = getIntent().getBooleanExtra(EXTRA_PICK_BIOS, false);
        if (pickBiosOnly) {
            if (savedInstanceState == null) pick(REQ_BIOS);
            return;
        }
        // Sync Folder's "Folder's" chosen last session: its saves put in place
        // now, before the game reads them (SyncFolder.applyImport), then on.
        if (SyncFolder.importWaiting(this)) {
            status.setText("Using the sync folder's saves...");
            new Thread(() -> {
                final String error = SyncFolder.applyImport(this);
                runOnUiThread(() -> {
                    if (error != null) status.setText(error);   // the player presses Play
                    else if (romOk) launchGame();
                    else boxViews.get(romBox).requestFocus();
                });
            }, "sync-import").start();
            return;
        }
        // Technical > Game asked for this screen: it stays up.
        if (new File(getFilesDir(), CHOOSE_NAME).delete()) {
            boxViews.get(romBox).requestFocus();
            return;
        }
        // Already provisioned from a previous run (the right ROM): go straight
        // to the game.
        if (romOk) launchGame();
        else boxViews.get(romBox).requestFocus();
    }

    @Override
    protected void onDestroy() {
        Game.selected = -1;
        super.onDestroy();
    }

    /** Whether game g's ROM is in its folder (its SHA-1) and the app plays it. */
    private boolean romIs(int g) {
        final File rom = Game.rom(this, g);
        return Game.hasProgram(this, g) && rom.exists() && Game.SHA1[g].equalsIgnoreCase(sha1(rom));
    }

    /** A ROM picked: told by its SHA-1, then into its own game's folder; that
     *  game is the one the boxes are for. Anything else is left out, and the
     *  ROMs in place stay as they are. */
    private void pickedRom(Uri uri) {
        final File tmp = new File(getFilesDir(), ROM_NAME + ".new");
        String message = "";
        try {
            copyToPrivate(uri, tmp.getName());
            final int g = Game.ofSha1(sha1(tmp));
            final boolean both = Game.programs(this) > 1;
            if (g < 0) {
                message = both ? "Wrong file: not the game's ROM (the USA or the Japanese version is needed)."
                               : "Wrong file: not this game's ROM (the USA version is needed).";
            } else if (!Game.hasProgram(this, g)) {
                message = "That is " + Game.NAME[g] + ": this app plays only the USA version.";
            } else {
                final File dir = Game.dir(this, g);
                dir.mkdirs();
                final File rom = Game.rom(this, g);
                rom.delete();
                if (!tmp.renameTo(rom)) {
                    message = "The ROM could not be loaded.";
                } else {
                    new File(dir, UNLINKED_NAME).delete();
                    game = g;
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "rom copy failed", e);
            message = "Copy failed: " + e.getMessage();
        }
        tmp.delete();
        refresh();   // the player presses Play
        // (After refresh, which clears the line when the boxes' ROM is right:
        // a file that was refused is said so even then.)
        status.setText(message);
    }

    private void pick(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            if (pickModOnly || pickBiosOnly || pickShadersOnly) finish();
            return;
        }
        if (request == REQ_MOD) {
            pickedMod(data.getData());
            return;
        }
        if (pickBiosOnly) {
            pickedBios(data.getData());
            return;
        }
        if (request == REQ_SHADERS) {
            importShaders(data.getData());
            return;
        }
        if (request == REQ_ROM) {
            pickedRom(data.getData());
            return;
        }
        try {
            copyToPrivate(data.getData(), BIOS_NAME);
            // A BIOS chosen here is the one to play with (launchGame puts the
            // marker back if the file is not the GBA BIOS).
            setNoBios(false);
        } catch (Exception e) {
            Log.e(TAG, "copy failed", e);
            status.setText("Copy failed: " + e.getMessage());
            return;
        }
        refresh();   // the player presses Play
    }

    /** A BIOS picked from the game's menu: kept only if it is the GBA BIOS, and
     *  then played with from the next start (the game asks to restart). */
    /** A folder of shaders: copied whole (on a thread: the full collection is
     *  ~3,000 files) into files/shaders/<its name>; the presets counted. */
    private void importShaders(final Uri tree) {
        status.setText("Copying shaders...");
        new Thread(() -> {
            String error = null, name = null;
            final int[] presets = {0};
            try {
                final String rootId = DocumentsContract.getTreeDocumentId(tree);
                name = displayName(DocumentsContract.buildDocumentUriUsingTree(tree, rootId));
                if (name == null || name.isEmpty() || name.startsWith(".")) name = "imported";
                final File dest = new File(new File(getFilesDir(), "shaders"), name.replace('/', '_'));
                if (!dest.isDirectory() && !dest.mkdirs()) throw new java.io.IOException("cannot make " + dest);
                copyTree(tree, rootId, dest, presets, 0);
            } catch (Exception e) {
                Log.e(TAG, "shader import failed", e);
                error = "The folder could not be copied: " + e.getMessage();
            }
            final String err = error, folder = name;
            runOnUiThread(() -> {
                Mods.nativeShadersImported(presets[0], folder, err);
                finish();
            });
        }, "shader-import").start();
    }

    private void copyTree(Uri tree, String docId, File dest, int[] presets, int depth) throws Exception {
        if (depth > 12) return;
        final Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        final String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                               DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                               DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                final String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null || name.startsWith(".") || name.contains("/")) continue;
                final File out = new File(dest, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!out.isDirectory() && !out.mkdirs()) continue;
                    copyTree(tree, id, out, presets, depth + 1);
                    continue;
                }
                try (InputStream in = getContentResolver().openInputStream(
                             DocumentsContract.buildDocumentUriUsingTree(tree, id));
                     OutputStream o = new FileOutputStream(out)) {
                    if (in == null) continue;
                    final byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
                if (name.toLowerCase(java.util.Locale.ROOT).endsWith(".glslp")) presets[0]++;
            }
        }
    }

    private void pickedBios(Uri uri) {
        String error = null;
        File tmp = new File(getFilesDir(), BIOS_NAME + ".new");
        try {
            copyToPrivate(uri, tmp.getName());
            if (!BIOS_SHA1.equalsIgnoreCase(sha1(tmp))) {
                error = displayName(uri) + " is not the GBA BIOS.";
            } else if (!tmp.renameTo(new File(getFilesDir(), BIOS_NAME))) {
                error = "The BIOS could not be loaded.";
            } else {
                setNoBios(false);
            }
        } catch (Exception e) {
            Log.e(TAG, "bios copy failed", e);
            error = "Copy failed: " + e.getMessage();
        }
        tmp.delete();
        Mods.nativeBiosPicked(error);
        finish();
    }

    /** A patch picked: checked for a patch's header, then files/mod/<its name>. */
    private void pickedMod(Uri uri) {
        String name = displayName(uri);
        String error = null;
        try {
            byte[] head = new byte[5];
            int n;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                n = in == null ? 0 : in.read(head);
            }
            String h = new String(head, 0, Math.max(n, 0), "ISO-8859-1");
            if (!(h.startsWith("PATCH") || h.startsWith("IPS32") || h.startsWith("UPS1") || h.startsWith("BPS1")))
                error = name + " is not an IPS, UPS or BPS patch.";
        } catch (Exception e) {
            error = "Cannot read " + name + ": " + e.getMessage();
        }
        if (error == null) {
            try {
                Mods.clear(this);
                File dir = new File(Game.activeDir(this), Mods.DIR);   // (the game's own)
                dir.mkdirs();
                copyTo(uri, new File(dir, name));
            } catch (Exception e) {
                Log.e(TAG, "mod copy failed", e);
                Mods.clear(this);
                error = "Copy failed: " + e.getMessage();
            }
        }
        refresh();
        if (!pickModOnly) {
            if (error != null) status.setText(error);
            return;
        }
        // Back to the game, which asks in its own window (kol::menu_mod_picked).
        Mods.nativePicked(error == null ? name : null, error);
        finish();
    }

    /** The picked file's own name, made safe for a file name. */
    private String displayName(Uri uri) {
        String name = null;
        try (android.database.Cursor c = getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception ignored) {
        }
        if (name == null || name.isEmpty()) name = "mod.patch";
        return name.replace('/', '_');
    }

    /**
     * Debug > Unlink ROM (dev builds; the game asked first, kol_menu): sets
     * the ROM copy aside and restarts, so the app starts on this screen, with
     * "Use the previous ROM". Saves, states and settings stay.
     */
    public static void unlinkRom() {
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            android.content.Context ctx = org.libsdl.app.SDL.getContext();
            if (!(ctx instanceof Activity) || ((Activity) ctx).isFinishing()) return;
            final Activity act = (Activity) ctx;
            File dir = Game.activeDir(act);   // (the running game's)
            File old = new File(dir, UNLINKED_NAME);
            old.delete();
            if (!new File(dir, ROM_NAME).renameTo(old)) {
                Log.e(TAG, "unlink: could not set the ROM aside");
                return;
            }
            RestartActivity.restart(act);
        });
    }

    private void copyToPrivate(Uri src, String destName) throws Exception {
        copyTo(src, new File(getFilesDir(), destName));
    }

    private void copyTo(Uri src, File dest) throws Exception {
        try (InputStream in = getContentResolver().openInputStream(src);
             OutputStream out = new FileOutputStream(dest)) {
            if (in == null) throw new IllegalStateException("cannot open source");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    /** files/no_bios, read by main_android.cpp: written when there is no real BIOS. */
    private void setNoBios(boolean on) {
        File marker = new File(getFilesDir(), NO_BIOS_NAME);
        if (!on) {
            marker.delete();
            return;
        }
        try {
            new FileOutputStream(marker).close();
        } catch (Exception e) {
            Log.e(TAG, "cannot write " + NO_BIOS_NAME, e);
        }
    }

    /** Each box from the files there, SHA-1 checked; Remove mod and Play to match. */
    private void refresh() {
        // The boxes' game: the one chosen, or with no ROM of its own in place
        // the one that has.
        final boolean[] have = new boolean[Game.COUNT];
        int games = 0;
        for (int g = 0; g < Game.COUNT; ++g) if (have[g] = romIs(g)) ++games;
        if (!have[game]) for (int g = 0; g < Game.COUNT; ++g) if (have[g]) { game = g; break; }
        Game.selected = game;
        title.setText(Game.NAME[game]);
        switchGame.setVisibility(games > 1 ? android.view.View.VISIBLE : android.view.View.GONE);
        if (games > 1) switchGame.setText("Switch to " + Game.NAME[(game + 1) % Game.COUNT]);

        File rom = Game.rom(this, game);
        romOk = have[game];
        if (!rom.exists()) show(romBox, '!', RED, "Required: select the game's ROM");
        else if (romOk) show(romBox, '\u2713', GREEN, "Loaded");
        else show(romBox, '!', RED, "Wrong file: not this game's ROM");

        File bios = new File(getFilesDir(), BIOS_NAME);
        String biosSha = bios.exists() ? sha1(bios) : null;
        biosOk = biosSha != null && biosSha.equalsIgnoreCase(BIOS_SHA1);
        if (!bios.exists()) show(biosBox, '?', GREY, "Optional: HLE BIOS used");
        else if (biosOk) show(biosBox, '\u2713', GREEN, "Loaded");
        else show(biosBox, '!', RED, "Not the GBA BIOS: HLE BIOS used");

        File mod = Mods.current(this);
        if (mod == null) show(modBox, '?', GREY, "Optional: an .ips, .ups or .bps patch");
        else show(modBox, '\u2713', GREEN, mod.getName());

        removeMod.setEnabled(mod != null);
        play.setEnabled(romOk);
        if (romOk) status.setText("");
    }

    private static String sha1(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest()) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return null;
        }
    }

    private void launchGame() {
        if (!romOk) {
            status.setText("Select the game's ROM first.");
            return;
        }
        // No GBA BIOS: the HLE BIOS (files/no_bios). With one, the marker is the
        // player's choice (Options > Technical > BIOS, or a BIOS picked here):
        // this runs on every start, the menu's restarts too, so it must not undo it.
        File bios = new File(getFilesDir(), BIOS_NAME);
        if (!(bios.exists() && BIOS_SHA1.equalsIgnoreCase(sha1(bios)))) setNoBios(true);
        // The game to play, for MainActivity's library (and the VS copy's).
        Game.choose(this, game);
        // A process holds one game's library: with the other one's loaded
        // here already, the app starts again (and comes back through this
        // screen, straight to the game).
        if (Game.loaded >= 0 && Game.loaded != game) {
            RestartActivity.restart(this);
            return;
        }
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }

    /**
     * Technical > Game (kol_menu): the app starts again on this screen, which
     * stays up, to take the other game's ROM or switch to it. Saves, states
     * and settings stay.
     */
    public static void chooseGame() {
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            android.content.Context ctx = org.libsdl.app.SDL.getContext();
            if (!(ctx instanceof Activity) || ((Activity) ctx).isFinishing()) return;
            final Activity act = (Activity) ctx;
            try {
                new FileOutputStream(new File(act.getFilesDir(), CHOOSE_NAME)).close();
            } catch (Exception e) {
                Log.e(TAG, "cannot write " + CHOOSE_NAME, e);
                return;
            }
            RestartActivity.restart(act);
        });
    }
}
