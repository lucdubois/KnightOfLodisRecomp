package com.koldecomp.game;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Pattern;

/**
 * Options > Technical > Sync Folder (Android, 2026-11-24; it replaced Logs >
 * Save): a folder the player picks, where the game's save, its save states
 * and its logs are copied whenever they change, so they can be kept, carried
 * to another device or sent with a bug report.
 *
 * What is copied, by its path under files/ (the same path in the folder):
 * totkol.sav and totkol.state1.., a mod's totkol-<8 hex>.sav and states, the
 * numbered states' folders ("states", "states-<8 hex>", with their pictures
 * and notes), and "logs". These are "the saves" below; the logs are copied
 * too but are never asked about or backed up. The Japanese game's saves are
 * the same kinds of files in files/japan (Game.java), copied to a "japan"
 * folder in the picked one (the logs are every game's, at the top only).
 * Every question, backup and import covers both games' saves as one set.
 *
 * Picking a folder that already holds saves of this game which differ from the
 * game's asks which to keep (kol_menu, menu_sync_ask): the folder's or the
 * game's. Whichever is chosen, the others go to a "backup-<date>" folder
 * inside the picked one. The game's: the folder's are moved there at once.
 * The folder's: they are copied into files/sync_import now, and put in place
 * at the next start, in SetupActivity before the game runs (applyImport), with
 * the game's own copied to the backup folder first: the running game keeps its
 * save in memory and writes it out, so it must not be replaced under it.
 *
 * Copying: a thread looks every few seconds at the files' sizes and times
 * against what it copied last (files/sync_record), and copies what changed.
 * A file still being written (changed under 1.5 s ago) waits for the next
 * look; a log is copied at most once a minute (it grows all session). A file
 * that changed IN THE FOLDER since this game wrote it (another device, a hand)
 * is moved to a backup folder before it is written over, never lost. A
 * numbered state deleted in the game is deleted in the folder too, if the
 * folder's copy is still the one made here.
 */
final class SyncFolder {
    private SyncFolder() {}

    private static final String TAG = "KoLSync";
    private static final String PREFS = "sync_folder";
    private static final String RECORD = "sync_record";   // files/: what was copied last
    private static final String IMPORT = "sync_import";   // files/: the folder's saves, for the next start
    private static final String BACKUP = "backup-";
    private static final String LOGS = "logs";
    private static final Pattern TOP = Pattern.compile("totkol(-[0-9a-f]{8})?\\.(sav|state[0-9]+)");
    private static final Pattern STATES = Pattern.compile("states(-[0-9a-f]{8})?");
    private static final long POLL_MS = 3000, SETTLE_MS = 1500, LOG_EVERY_MS = 60_000;
    private static final String OCTET = "application/octet-stream";

    private static Context app;
    private static final Object PASS = new Object();   // one pass, link or import at a time
    private static final Object WAKE = new Object();
    private static int generation;                      // under WAKE: a worker runs while its number is current
    private static boolean wake;                        // under WAKE
    private static Thread worker;                       // under WAKE
    private static String lastError;                    // what the game was told
    private static final Map<String, Long> logCopied = new HashMap<>();

    // ── Where things are ────────────────────────────────────────────────

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static Uri linkedTree(Context c) {
        final String s = prefs(c).getString("tree", null);
        return s == null ? null : Uri.parse(s);
    }

    static String linkedName(Context c) {
        return prefs(c).getString("name", null);
    }

    private static boolean isLog(String rel) {
        return rel.startsWith(LOGS + "/");
    }

    private static boolean isState(String rel) {
        return rel.matches(".*\\.state[0-9]*");
    }

    /** files/'s saves (relative path -> file), every game's, and its logs if asked. */
    static TreeMap<String, File> appFiles(File base, boolean logs) {
        final TreeMap<String, File> out = new TreeMap<>();
        appGameFiles(base, "", logs, out);
        for (int g = 0; g < Game.COUNT; ++g) {
            final String folder = Game.folder(g);
            if (!folder.isEmpty()) appGameFiles(new File(base, folder), folder + "/", false, out);
        }
        return out;
    }

    /** One game's saves, in dir, under prefix ("" or "japan/"). */
    private static void appGameFiles(File dir, String prefix, boolean logs, TreeMap<String, File> out) {
        final File[] top = dir.listFiles();
        if (top == null) return;
        for (File f : top) {
            final String n = f.getName();
            if (f.isFile() && TOP.matcher(n).matches()) {
                out.put(prefix + n, f);
            } else if (f.isDirectory() && (STATES.matcher(n).matches() || (logs && n.equals(LOGS)))) {
                final File[] in = f.listFiles();
                if (in != null)
                    for (File g : in)
                        if (g.isFile() && !g.getName().startsWith(".")) out.put(prefix + n + "/" + g.getName(), g);
            }
        }
    }

    /** A file or folder in the picked folder. */
    static final class Doc {
        final String id, parent;
        final long size, mtime;
        final boolean dir;

        Doc(String id, String parent, long size, long mtime, boolean dir) {
            this.id = id;
            this.parent = parent;
            this.size = size;
            this.mtime = mtime;
            this.dir = dir;
        }
    }

    private static Uri uri(Uri tree, String id) {
        return DocumentsContract.buildDocumentUriUsingTree(tree, id);
    }

    private static LinkedHashMap<String, Doc> children(ContentResolver cr, Uri tree, String parent) throws IOException {
        final LinkedHashMap<String, Doc> out = new LinkedHashMap<>();
        try (Cursor c = cr.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent),
                new String[] {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
                              Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED}, null, null, null)) {
            if (c == null) throw new IOException("the folder cannot be read");
            while (c.moveToNext()) {
                final String name = c.getString(1);
                if (name == null || name.isEmpty() || name.contains("/")) continue;
                out.put(name, new Doc(c.getString(0), parent, c.isNull(3) ? -1 : c.getLong(3),
                                      c.isNull(4) ? -1 : c.getLong(4), Document.MIME_TYPE_DIR.equals(c.getString(2))));
            }
        }
        return out;
    }

    /** The folder's saves (relative path -> doc), every game's, and its logs if
     *  asked. Its subfolders read go in dirs, the folder itself as "". */
    static TreeMap<String, Doc> folderFiles(ContentResolver cr, Uri tree, Map<String, Doc> dirs, boolean logs)
            throws IOException {
        final String root = DocumentsContract.getTreeDocumentId(tree);
        final TreeMap<String, Doc> out = new TreeMap<>();
        dirs.put("", new Doc(root, null, 0, 0, true));
        folderGameFiles(cr, tree, root, "", dirs, logs, out);
        return out;
    }

    /** One game's saves in the folder's doc id, under prefix ("" or "japan/");
     *  at the top, the other games' folders too. */
    private static void folderGameFiles(ContentResolver cr, Uri tree, String id, String prefix, Map<String, Doc> dirs,
                                        boolean logs, TreeMap<String, Doc> out) throws IOException {
        final List<String> sub = new ArrayList<>();
        final List<String> games = new ArrayList<>();
        for (Map.Entry<String, Doc> e : children(cr, tree, id).entrySet()) {
            final String n = e.getKey();
            final Doc d = e.getValue();
            if (!d.dir && TOP.matcher(n).matches()) {
                out.put(prefix + n, d);
            } else if (d.dir && (STATES.matcher(n).matches() || (logs && n.equals(LOGS)))) {
                dirs.put(prefix + n, d);
                sub.add(prefix + n);
            } else if (d.dir && prefix.isEmpty() && Game.isFolder(n)) {
                dirs.put(n, d);
                games.add(n);
            }
        }
        for (String rel : sub)
            for (Map.Entry<String, Doc> e : children(cr, tree, dirs.get(rel).id).entrySet())
                if (!e.getValue().dir && !e.getKey().startsWith(".")) out.put(rel + "/" + e.getKey(), e.getValue());
        for (String g : games) folderGameFiles(cr, tree, dirs.get(g).id, g + "/", dirs, false, out);
    }

    private static Doc stat(ContentResolver cr, Uri tree, String id, String parent) {
        try (Cursor c = cr.query(uri(tree, id), new String[] {Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED},
                                 null, null, null)) {
            if (c != null && c.moveToFirst())
                return new Doc(id, parent, c.isNull(0) ? -1 : c.getLong(0), c.isNull(1) ? -1 : c.getLong(1), false);
        } catch (Exception e) {
            Log.w(TAG, "stat " + id + ": " + e);
        }
        return new Doc(id, parent, -1, -1, false);
    }

    /** The subfolder at rel ("states", "backup-.../states"), made if missing. */
    private static Doc dir(ContentResolver cr, Uri tree, Map<String, Doc> dirs, String rel) throws IOException {
        Doc d = dirs.get(rel);
        if (d != null) return d;
        final int cut = rel.lastIndexOf('/');
        final Doc parent = dir(cr, tree, dirs, cut < 0 ? "" : rel.substring(0, cut));
        final String name = rel.substring(cut + 1);
        final Doc there = children(cr, tree, parent.id).get(name);
        if (there != null && there.dir) {
            dirs.put(rel, there);
            return there;
        }
        final Uri made = DocumentsContract.createDocument(cr, uri(tree, parent.id), Document.MIME_TYPE_DIR, name);
        if (made == null) throw new IOException("cannot make the folder " + rel);
        d = new Doc(DocumentsContract.getDocumentId(made), parent.id, 0, 0, true);
        dirs.put(rel, d);
        return d;
    }

    private static String parentOf(String rel) {
        final int cut = rel.lastIndexOf('/');
        return cut < 0 ? "" : rel.substring(0, cut);
    }

    private static String nameOf(String rel) {
        return rel.substring(rel.lastIndexOf('/') + 1);
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        final byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }

    /** src written to rel in the folder, over doc (its contents replaced) or as a new file. */
    private static Doc writeOut(ContentResolver cr, Uri tree, Map<String, Doc> dirs, String rel, File src, Doc doc)
            throws IOException {
        final Doc parent = dir(cr, tree, dirs, parentOf(rel));
        Uri u;
        if (doc != null) {
            u = uri(tree, doc.id);
        } else {
            // octet-stream: the provider keeps the name as given (a text type
            // could get ".txt" added).
            u = DocumentsContract.createDocument(cr, uri(tree, parent.id), OCTET, nameOf(rel));
            if (u == null) throw new IOException("cannot write " + rel);
        }
        try (InputStream in = new FileInputStream(src); OutputStream out = cr.openOutputStream(u, "wt")) {
            if (out == null) throw new IOException("cannot write " + rel);
            copy(in, out);
        }
        return stat(cr, tree, DocumentsContract.getDocumentId(u), parent.id);
    }

    private static void copyIn(ContentResolver cr, Uri tree, Doc doc, File dest) throws IOException {
        final File parent = dest.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot make " + parent);
        try (InputStream in = cr.openInputStream(uri(tree, doc.id)); OutputStream out = new FileOutputStream(dest)) {
            if (in == null) throw new IOException("cannot read " + dest.getName());
            copy(in, out);
        }
    }

    private static int fill(InputStream in, byte[] buf) throws IOException {
        int got = 0, n;
        while (got < buf.length && (n = in.read(buf, got, buf.length - got)) > 0) got += n;
        return got;
    }

    private static boolean sameStreams(InputStream a, InputStream b) throws IOException {
        final byte[] x = new byte[1 << 16], y = new byte[1 << 16];
        while (true) {
            final int n = fill(a, x), m = fill(b, y);
            if (n != m) return false;
            if (n == 0) return true;
            for (int i = 0; i < n; ++i)
                if (x[i] != y[i]) return false;
        }
    }

    /** The folder's doc holds the same bytes as the file. */
    private static boolean same(ContentResolver cr, Uri tree, Doc doc, File f) {
        if (doc.size >= 0 && doc.size != f.length()) return false;
        try (InputStream a = cr.openInputStream(uri(tree, doc.id)); InputStream b = new FileInputStream(f)) {
            return a != null && sameStreams(a, b);
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean same(File f, File g) {
        if (f.length() != g.length()) return false;
        try (InputStream a = new FileInputStream(f); InputStream b = new FileInputStream(g)) {
            return sameStreams(a, b);
        } catch (Exception e) {
            return false;
        }
    }

    private static String backupName() {
        return BACKUP + new SimpleDateFormat("yyyy-MM-dd-HHmmss", Locale.US).format(new Date());
    }

    /** The folder's doc at rel moved to backup/rel (copied and deleted where the folder cannot move). */
    private static void toBackup(ContentResolver cr, Uri tree, Map<String, Doc> dirs, String backup, String rel, Doc doc)
            throws IOException {
        final String where = parentOf(rel);
        final Doc target = dir(cr, tree, dirs, where.isEmpty() ? backup : backup + "/" + where);
        try {
            if (DocumentsContract.moveDocument(cr, uri(tree, doc.id), uri(tree, doc.parent), uri(tree, target.id)) != null)
                return;
        } catch (Exception e) {
            Log.i(TAG, "move refused (" + e + "), copying " + rel);
        }
        final Uri made = DocumentsContract.createDocument(cr, uri(tree, target.id), OCTET, nameOf(rel));
        if (made == null) throw new IOException("cannot back up " + rel);
        try (InputStream in = cr.openInputStream(uri(tree, doc.id)); OutputStream out = cr.openOutputStream(made, "wt")) {
            if (in == null || out == null) throw new IOException("cannot back up " + rel);
            copy(in, out);
        }
        DocumentsContract.deleteDocument(cr, uri(tree, doc.id));
    }

    private static String folderName(ContentResolver cr, Uri tree) {
        try (Cursor c = cr.query(uri(tree, DocumentsContract.getTreeDocumentId(tree)),
                                 new String[] {Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (Exception ignored) {
        }
        return "the folder";
    }

    private static String message(Exception e) {
        if (e instanceof SecurityException) return "the folder may no longer be reached; choose it again";
        return e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
    }

    private static void deleteTree(File f) {
        final File[] in = f.listFiles();
        if (in != null) for (File g : in) deleteTree(g);
        f.delete();
    }

    // ── What was copied last ────────────────────────────────────────────

    /** One file as copied: the game's size and time, the folder's after the copy. */
    static final class Rec {
        long appSize, appMtime, docSize, docMtime;

        static Rec of(File f, Doc d) {
            final Rec r = new Rec();
            r.appSize = f.length();
            r.appMtime = f.lastModified();
            r.docSize = d.size;
            r.docMtime = d.mtime;
            return r;
        }
    }

    /** The record, for this folder (empty if it was another's). */
    private static TreeMap<String, Rec> readRecord(File base, Uri tree) {
        final TreeMap<String, Rec> out = new TreeMap<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(base, RECORD)))) {
            if (!tree.toString().equals(r.readLine())) return out;
            String line;
            while ((line = r.readLine()) != null) {
                final String[] f = line.split("\t");
                if (f.length != 5) continue;
                final Rec rec = new Rec();
                rec.appSize = Long.parseLong(f[1]);
                rec.appMtime = Long.parseLong(f[2]);
                rec.docSize = Long.parseLong(f[3]);
                rec.docMtime = Long.parseLong(f[4]);
                out.put(f[0], rec);
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    private static void writeRecord(File base, Uri tree, Map<String, Rec> rec) {
        final File tmp = new File(base, RECORD + ".tmp");
        try (PrintWriter w = new PrintWriter(new FileOutputStream(tmp))) {
            w.println(tree.toString());
            for (Map.Entry<String, Rec> e : rec.entrySet()) {
                final Rec r = e.getValue();
                w.println(e.getKey() + "\t" + r.appSize + "\t" + r.appMtime + "\t" + r.docSize + "\t" + r.docMtime);
            }
        } catch (Exception e) {
            Log.e(TAG, "record not written", e);
            return;
        }
        if (!tmp.renameTo(new File(base, RECORD))) Log.e(TAG, "record not renamed");
    }

    // ── To the game (kol_menu) ──────────────────────────────────────────

    private static void report(String name, String error) {
        try {
            Mods.nativeSyncState(name, error);
        } catch (UnsatisfiedLinkError e) {
            // Before the game's library (SetupActivity): MainActivity says it.
        }
    }

    private static void busy(String text) {
        try {
            Mods.nativeSyncBusy(text);
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, text);
        }
    }

    private static void done(String text, boolean restart) {
        try {
            Mods.nativeSyncDone(text, restart);
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, text);
        }
    }

    // ── The copying ─────────────────────────────────────────────────────

    /** MainActivity's start: the game told the folder, the copying started if
     *  one is set, and an import's note shown once the game runs. */
    static void start(Context ctx) {
        app = ctx.getApplicationContext();
        final Uri tree = linkedTree(app);
        report(tree == null ? null : linkedName(app), null);
        final String note = prefs(app).getString("note", null);
        if (note != null) {
            prefs(app).edit().remove("note").apply();
            new Handler(Looper.getMainLooper()).postDelayed(() -> done(note, false), 4000);
        }
        if (tree != null) startWorker();
    }

    private static void startWorker() {
        synchronized (WAKE) {
            if (worker != null && worker.isAlive()) {
                wake = true;
                WAKE.notifyAll();
                return;
            }
            final int mine = ++generation;
            worker = new Thread(() -> loop(mine), "sync-folder");
            worker.setDaemon(true);
            worker.start();
        }
    }

    /** A look now (the game going out of sight). */
    static void nudge() {
        synchronized (WAKE) {
            wake = true;
            WAKE.notifyAll();
        }
    }

    private static void stopWorker() {
        synchronized (WAKE) {
            ++generation;
            worker = null;
            WAKE.notifyAll();
        }
    }

    private static void loop(int mine) {
        while (true) {
            synchronized (WAKE) {
                if (!wake && generation == mine) {
                    try {
                        WAKE.wait(POLL_MS);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                wake = false;
                if (generation != mine) return;
            }
            final Uri tree = linkedTree(app);
            if (tree == null) return;
            synchronized (PASS) {
                pass(tree);
            }
        }
    }

    /** One look: what changed in the game since the last copy goes to the folder. */
    private static void pass(Uri tree) {
        final File base = app.getFilesDir();
        final ContentResolver cr = app.getContentResolver();
        final long now = System.currentTimeMillis();
        final TreeMap<String, Rec> rec = readRecord(base, tree);
        final TreeMap<String, File> files = appFiles(base, true);
        final List<String> copy = new ArrayList<>();
        for (Map.Entry<String, File> e : files.entrySet()) {
            final String rel = e.getKey();
            final File f = e.getValue();
            final Rec r = rec.get(rel);
            if (r != null && r.appSize == f.length() && r.appMtime == f.lastModified()) continue;
            if (isLog(rel)) {
                final Long last = logCopied.get(rel);
                if (last != null && now - last < LOG_EVERY_MS) continue;
            } else if (now - f.lastModified() < SETTLE_MS) {
                continue;   // still being written: the next look
            }
            copy.add(rel);
        }
        final List<String> gone = new ArrayList<>();
        for (String rel : rec.keySet())
            if (!files.containsKey(rel) && !isLog(rel)) gone.add(rel);
        if (copy.isEmpty() && gone.isEmpty()) return;

        String error = null;
        String backup = null;
        try {
            final Map<String, Doc> dirs = new HashMap<>();
            final TreeMap<String, Doc> there = folderFiles(cr, tree, dirs, true);
            for (String rel : copy) {
                final File f = files.get(rel);
                // Taken before the copy: a write during it shows at the next look.
                final long size = f.length(), mtime = f.lastModified();
                Doc doc = there.get(rel);
                final Rec r = rec.get(rel);
                if (doc != null && !isLog(rel) && (r == null || doc.size != r.docSize || doc.mtime != r.docMtime)) {
                    // Changed in the folder since the game wrote it: kept.
                    if (backup == null) backup = backupName();
                    toBackup(cr, tree, dirs, backup, rel, doc);
                    doc = null;
                }
                final Doc wrote = writeOut(cr, tree, dirs, rel, f, doc);
                final Rec n = new Rec();
                n.appSize = size;
                n.appMtime = mtime;
                n.docSize = wrote.size;
                n.docMtime = wrote.mtime;
                rec.put(rel, n);
                if (isLog(rel)) logCopied.put(rel, now);
            }
            for (String rel : gone) {
                final Doc doc = there.get(rel);
                final Rec r = rec.remove(rel);
                // Deleted in the game (a numbered state): in the folder too, if it is still the copy made here.
                if (doc != null && doc.size == r.docSize && doc.mtime == r.docMtime)
                    DocumentsContract.deleteDocument(cr, uri(tree, doc.id));
            }
        } catch (Exception e) {
            Log.e(TAG, "copy failed", e);
            error = message(e);
        }
        if (backup != null) Log.i(TAG, "files changed in the folder were moved to " + backup);
        Log.i(TAG, "copied " + copy.size() + ", deleted " + gone.size() + (error != null ? ", failed: " + error : ""));
        writeRecord(base, tree, rec);
        if (error == null ? lastError != null : !error.equals(lastError)) {
            lastError = error;
            report(linkedName(app), error);
        }
    }

    // ── A folder picked (SetupActivity) ─────────────────────────────────

    /** A picked folder that holds saves differing from the game's, awaiting the player's answer. */
    private static final class Pending {
        Uri tree;
        String name;
        TreeMap<String, Doc> theirs;   // the folder's saves that the game does not hold alike
        TreeMap<String, Rec> same;     // those both hold alike
    }
    private static Pending pending;

    private static void link(Uri tree, String name, TreeMap<String, Rec> same, boolean startNow) {
        final Uri old = linkedTree(app);
        prefs(app).edit().putString("tree", tree.toString()).putString("name", name).commit();
        writeRecord(app.getFilesDir(), tree, same);
        if (old != null && !old.equals(tree)) release(old);
        lastError = null;
        logCopied.clear();
        if (startNow) {
            report(name, null);
            startWorker();
            nudge();
        }
    }

    private static void release(Uri tree) {
        try {
            app.getContentResolver().releasePersistableUriPermission(
                    tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception ignored) {
        }
    }

    /** After the picker, on FolderPickActivity's thread (the game showing its
     *  busy box): the folder kept, its saves compared with the game's; linked
     *  at once, or the game asks. */
    static void picked(Context ctx, Uri tree) {
        app = ctx.getApplicationContext();
        final ContentResolver cr = app.getContentResolver();
        try {
            cr.takePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception e) {
            Log.e(TAG, "permission", e);
            done("This folder cannot be used: " + message(e) + ".", false);
            return;
        }
        synchronized (PASS) {
            try {
                final String name = folderName(cr, tree);
                final TreeMap<String, Doc> there = folderFiles(cr, tree, new HashMap<>(), false);
                final TreeMap<String, File> ours = appFiles(app.getFilesDir(), false);
                final TreeMap<String, Doc> theirs = new TreeMap<>();
                final TreeMap<String, Rec> same = new TreeMap<>();
                for (Map.Entry<String, Doc> e : there.entrySet()) {
                    final File f = ours.get(e.getKey());
                    if (f != null && same(cr, tree, e.getValue(), f)) same.put(e.getKey(), Rec.of(f, e.getValue()));
                    else theirs.put(e.getKey(), e.getValue());
                }
                if (theirs.isEmpty()) {
                    link(tree, name, same, true);
                    done("Copying to " + name + " whenever they change: the save, the save states and the logs.",
                         false);
                    return;
                }
                final Pending p = new Pending();
                p.tree = tree;
                p.name = name;
                p.theirs = theirs;
                p.same = same;
                pending = p;
                final List<Long> theirTimes = new ArrayList<>();
                for (Doc d : there.values()) theirTimes.add(d.mtime);
                final StringBuilder text = new StringBuilder();
                text.append(name).append(" already has this game's saves: ")
                    .append(describe(new ArrayList<>(there.keySet()), theirTimes))
                    .append(". The game has ");
                final List<Long> ourTimes = new ArrayList<>();
                for (File f : ours.values()) ourTimes.add(f.lastModified());
                text.append(describe(new ArrayList<>(ours.keySet()), ourTimes))
                    .append(". Keep the folder's or the game's? The others go to a backup folder in ")
                    .append(name).append(".");
                try {
                    Mods.nativeSyncAsk(text.toString());
                } catch (UnsatisfiedLinkError e) {
                    pending = null;
                }
            } catch (Exception e) {
                Log.e(TAG, "pick", e);
                if (!tree.equals(linkedTree(app))) release(tree);
                done("This folder cannot be used: " + message(e) + ".", false);
            }
        }
    }

    /** "a save from Nov 20, 14:02 and 3 save states" */
    private static String describe(List<String> rels, List<Long> times) {
        int saves = 0, states = 0;
        long newest = -1;
        for (int i = 0; i < rels.size(); ++i) {
            final String rel = rels.get(i);
            if (rel.endsWith(".sav")) {
                ++saves;
                newest = Math.max(newest, times.get(i));
            } else if (isState(rel)) {
                ++states;
            }
        }
        final String when = newest > 0
                ? " from " + new SimpleDateFormat("MMM d, HH:mm", Locale.US).format(new Date(newest)) : "";
        final String s = saves == 0 ? "no save"
                : saves == 1 ? "a save" + when : saves + " saves, the newest" + when;
        if (states == 0) return s;
        return s + " and " + states + (states == 1 ? " save state" : " save states");
    }

    /** The player's answer (kol_menu): 0 the folder's, 1 the game's, else cancel. */
    static void answer(int which) {
        final Pending p = pending;
        pending = null;
        if (p == null) return;
        if (which != 0 && which != 1) {
            if (!p.tree.equals(linkedTree(app))) release(p.tree);
            return;
        }
        busy(which == 1 ? "Moving the folder's saves to a backup folder..." : "Copying the folder's saves...");
        new Thread(() -> {
            synchronized (PASS) {
                if (which == 1) keepGames(p);
                else stageFolders(p);
            }
        }, "sync-folder-link").start();
    }

    /** The game's kept: the folder's differing saves moved to a backup folder there, then linked. */
    private static void keepGames(Pending p) {
        final ContentResolver cr = app.getContentResolver();
        try {
            final String backup = backupName();
            final Map<String, Doc> dirs = new HashMap<>();
            folderFiles(cr, p.tree, dirs, false);
            for (Map.Entry<String, Doc> e : p.theirs.entrySet()) toBackup(cr, p.tree, dirs, backup, e.getKey(), e.getValue());
            link(p.tree, p.name, p.same, true);
            done("Copying to " + p.name + " whenever they change. What it held before is in " + backup + " there.",
                 false);
        } catch (Exception e) {
            Log.e(TAG, "keep the game's", e);
            if (!p.tree.equals(linkedTree(app))) release(p.tree);
            done("The folder could not be set: " + message(e) + ".", false);
        }
    }

    /** The folder's chosen: its saves copied into files/sync_import, put in place at the next start. */
    private static void stageFolders(Pending p) {
        final ContentResolver cr = app.getContentResolver();
        final File stage = new File(app.getFilesDir(), IMPORT);
        deleteTree(stage);
        try {
            final File into = new File(stage, "files");
            for (Map.Entry<String, Doc> e : folderFiles(cr, p.tree, new HashMap<>(), false).entrySet())
                copyIn(cr, p.tree, e.getValue(), new File(into, e.getKey()));
            try (PrintWriter w = new PrintWriter(new FileOutputStream(new File(stage, "plan")))) {
                w.println(p.tree.toString());
                w.println(p.name);
            }
            done("The game plays with the folder's saves from its next start. Its own go to a backup folder in "
                 + p.name + ". Restart now?", true);
        } catch (Exception e) {
            Log.e(TAG, "use the folder's", e);
            deleteTree(stage);
            if (!p.tree.equals(linkedTree(app))) release(p.tree);
            done("The folder's saves could not be copied: " + message(e) + ".", false);
        }
    }

    /** Options > Technical > Sync Folder > Stop: no more copies (the folder keeps what it has). */
    static void stop() {
        if (app == null) return;
        synchronized (PASS) {
            stopWorker();
            final Uri tree = linkedTree(app);
            prefs(app).edit().remove("tree").remove("name").commit();
            new File(app.getFilesDir(), RECORD).delete();
            deleteTree(new File(app.getFilesDir(), IMPORT));
            if (tree != null) release(tree);
            lastError = null;
        }
        report(null, null);
    }

    // ── The folder's saves put in place (SetupActivity, before the game) ─

    /** Every file stageFolders copied in (relative path -> file): the paths
     *  folderFiles gave, so "japan/states/0001.state" and the like. */
    private static void stagedFiles(File dir, String prefix, TreeMap<String, File> out) {
        final File[] in = dir.listFiles();
        if (in == null) return;
        for (File f : in) {
            if (f.isFile()) out.put(prefix + f.getName(), f);
            else if (f.isDirectory()) stagedFiles(f, prefix + f.getName() + "/", out);
        }
    }

    static boolean importWaiting(Context c) {
        return new File(new File(c.getFilesDir(), IMPORT), "plan").isFile();
    }

    /**
     * The folder's saves, staged by stageFolders, put in place before the game
     * runs: the game's own that the folder's do not repeat are copied to a
     * backup folder in the folder first; then the staged files replace them
     * and the folder is linked. Null when done; else why not (the game's own
     * are then left as they were). On a thread.
     */
    static String applyImport(Context ctx) {
        app = ctx.getApplicationContext();
        final File base = app.getFilesDir();
        final File stage = new File(base, IMPORT);
        final ContentResolver cr = app.getContentResolver();
        synchronized (PASS) {
            try {
                final Uri tree;
                final String name;
                try (BufferedReader r = new BufferedReader(new FileReader(new File(stage, "plan")))) {
                    tree = Uri.parse(r.readLine());
                    name = r.readLine();
                }
                boolean allowed = false;
                for (UriPermission u : cr.getPersistedUriPermissions())
                    allowed |= u.getUri().equals(tree) && u.isWritePermission();
                if (!allowed) throw new IOException("the folder may no longer be reached; choose it again");

                final TreeMap<String, File> staged = new TreeMap<>();
                stagedFiles(new File(stage, "files"), "", staged);
                final TreeMap<String, File> ours = appFiles(base, false);

                // The game's own first, to the backup folder.
                String backup = null;
                final Map<String, Doc> dirs = new HashMap<>();
                folderFiles(cr, tree, dirs, false);
                for (Map.Entry<String, File> e : ours.entrySet()) {
                    final File s = staged.get(e.getKey());
                    if (s != null && same(s, e.getValue())) continue;
                    if (backup == null) backup = backupName();
                    writeOut(cr, tree, dirs, backup + "/" + e.getKey(), e.getValue(), null);
                }
                // Then the folder's in their place (a rename replaces), and the rest of the game's gone.
                for (Map.Entry<String, File> e : staged.entrySet()) {
                    final File dest = new File(base, e.getKey());
                    final File parent = dest.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                        throw new IOException("cannot make " + parent);
                    if (!e.getValue().renameTo(dest)) throw new IOException("cannot put " + e.getKey() + " in place");
                }
                for (Map.Entry<String, File> e : ours.entrySet())
                    if (!staged.containsKey(e.getKey())) e.getValue().delete();

                final TreeMap<String, Doc> there = folderFiles(cr, tree, new HashMap<>(), false);
                final TreeMap<String, Rec> same = new TreeMap<>();
                for (String rel : staged.keySet()) {
                    final Doc d = there.get(rel);
                    if (d != null) same.put(rel, Rec.of(new File(base, rel), d));
                }
                link(tree, name, same, false);
                deleteTree(stage);
                prefs(app).edit().putString("note", "Playing with the saves from " + name + "."
                        + (backup != null ? " The game's earlier ones are in " + backup + " there." : "")).commit();
                return null;
            } catch (Exception e) {
                Log.e(TAG, "import", e);
                deleteTree(stage);
                return "The folder's saves were not used: " + message(e) + ". The game's own are kept.";
            }
        }
    }
}
