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
 * gate). The BIOS is optional: without one, or with a file that is not the
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

    // main_android.cpp looks for exactly these names.
    private static final String ROM_NAME  = "totkol.gba";
    private static final String BIOS_NAME = "gba_bios.bin";
    // Present = play without a BIOS; main_android.cpp then passes --no-bios.
    private static final String NO_BIOS_NAME = "no_bios";
    // Debug > Unlink ROM (unlinkRom): the ROM copy set aside, so the next
    // start is this screen; "Use the previous ROM" puts it back.
    private static final String UNLINKED_NAME = "totkol.gba.unlinked";

    // Verified in KnightOfLodisRecomp/CLAUDE.md.
    private static final String ROM_SHA1  = "85a46320a31f16f5c6dda27fc094edd078dbe43d";
    private static final String BIOS_SHA1 = "300c20df6731a33952ded8c436f7f186d25d3492";

    private static final int REQ_ROM  = 1;
    private static final int REQ_BIOS = 2;
    private static final int REQ_MOD  = 3;
    private static final int REQ_SHADERS = 4;
    private static final int REQ_SYNC = 5;
    // Options > Technical > Sync Folder (Mods.syncPick): a folder picked, then
    // SyncFolder.picked compares it with the game's saves (on a thread).
    static final String EXTRA_SYNC_PICK = "sync_pick";
    private boolean syncPickOnly;
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
    private TextView status;     // a copy's error, a missing ROM
    private Button removeMod, play;
    private boolean romOk, biosOk;

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

        TextView title = new TextView(this);
        title.setText("Tactics Ogre: The Knight of Lodis");
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
        if (new File(getFilesDir(), UNLINKED_NAME).exists()) {
            Button relink = new Button(this);
            relink.setText("Use the previous ROM");
            relink.setOnClickListener(v -> {
                File rom = new File(getFilesDir(), ROM_NAME);
                if (rom.exists() || new File(getFilesDir(), UNLINKED_NAME).renameTo(rom)) {
                    root.removeView(v);
                    refresh();
                }
            });
            root.addView(relink);
        }

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
        syncPickOnly = getIntent().getBooleanExtra(EXTRA_SYNC_PICK, false);
        if (syncPickOnly) {
            if (savedInstanceState == null) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                // Kept across restarts (SyncFolder takes the permission).
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                           | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                // Opens in Documents, where "Use this folder" is allowed:
                // Android 11 and later refuse the storage's top and Download
                // itself as a picked folder.
                if (android.os.Build.VERSION.SDK_INT >= 26) {
                    i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                            "com.android.externalstorage.documents", "primary:Documents"));
                }
                try {
                    startActivityForResult(i, REQ_SYNC);
                } catch (Exception e) {
                    Mods.nativeSyncDone("This device has no folder picker.", false);
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
        // Already provisioned from a previous run (the right ROM): go straight
        // to the game.
        if (romOk) launchGame();
        else boxViews.get(romBox).requestFocus();
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
            if (pickModOnly || pickBiosOnly || pickShadersOnly || syncPickOnly) finish();
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
        if (request == REQ_SYNC) {
            final Uri tree = data.getData();
            status.setText("Looking in the folder...");
            new Thread(() -> {
                SyncFolder.picked(this, tree);
                runOnUiThread(this::finish);
            }, "sync-pick").start();
            return;
        }
        String name = (request == REQ_ROM) ? ROM_NAME : BIOS_NAME;
        try {
            copyToPrivate(data.getData(), name);
            if (request == REQ_ROM) new File(getFilesDir(), UNLINKED_NAME).delete();
            // A BIOS chosen here is the one to play with (launchGame puts the
            // marker back if the file is not the GBA BIOS).
            if (request == REQ_BIOS) setNoBios(false);
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
                File dir = new File(getFilesDir(), Mods.DIR);
                dir.mkdirs();
                copyToPrivate(uri, Mods.DIR + "/" + name);
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
            File dir = act.getFilesDir();
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
        File dest = new File(getFilesDir(), destName);
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
        File rom = new File(getFilesDir(), ROM_NAME);
        String romSha = rom.exists() ? sha1(rom) : null;
        romOk = romSha != null && romSha.equalsIgnoreCase(ROM_SHA1);
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
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }
}
