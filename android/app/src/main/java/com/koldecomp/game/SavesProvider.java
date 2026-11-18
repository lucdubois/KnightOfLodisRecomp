package com.koldecomp.game;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * The game's save and save states, shown in Android's Files app as a location
 * named after the game, so a player can copy them out, or put one in, on the
 * device itself.
 *
 * The files stay where the game keeps them (the app's private files folder);
 * this only lets the Files app see some of them: at the top, files named like
 * a save or a state (totkol.sav, totkol.state1, anything.sav, and the ".bak" a
 * replaced one leaves), and the numbered states' folder with all it holds:
 * "states", and "states-<8 hex>" for each mod played (a mod keeps its own save,
 * totkol-<8 hex>.sav, and its own states). The ROM, the BIOS and the settings
 * are never shown.
 *
 * A file put in under a name that exists replaces it; the one that was there
 * is kept beside it as "<name>.bak". The game reads totkol.sav when it starts,
 * so a save is replaced with the game closed; a state is read when it is loaded.
 */
public class SavesProvider extends DocumentsProvider {
    private static final String ROOT_ID = "saves";
    // The top folder's document id; every other id is a path under files/.
    private static final String TOP = "top";
    private static final String BAK = ".bak";

    private static final String[] ROOT_COLS = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_ICON, Root.COLUMN_TITLE,
        Root.COLUMN_SUMMARY, Root.COLUMN_DOCUMENT_ID,
    };
    private static final String[] DOC_COLS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE,
    };

    @Override
    public boolean onCreate() { return true; }

    private File base() { return getContext().getFilesDir(); }

    private String authority() { return getContext().getPackageName() + ".documents"; }

    /** A top-level file the Files app may see: a save or a state, or its ".bak". */
    private static boolean saveName(String name) {
        return name.matches("[^/]+\\.(sav|state[0-9]*)(\\.bak)?");
    }

    /** A numbered states' folder at the top: the game's, or a mod's (kol_states.cpp). */
    private static boolean statesDir(String name) {
        return name.matches("states(-[0-9a-f]{8})?");
    }

    /** May this path under files/ be shown? Never anything with a ".." in it. */
    private static boolean shown(String id) {
        if (id.equals(TOP)) return true;
        String[] parts = id.split("/", -1);
        for (String part : parts)
            if (part.isEmpty() || part.equals(".") || part.equals("..")) return false;
        if (statesDir(parts[0])) return true;
        return parts.length == 1 && saveName(id);
    }

    /** May a file of this name be made in (or renamed within) this folder? */
    private static boolean nameAllowed(String parentId, String name) {
        if (name.contains("/") || !shown(childId(parentId, name))) return false;
        // At the top only a save or a state: never a file in a states folder's name.
        return !parentId.equals(TOP) || saveName(name);
    }

    private File file(String id) throws FileNotFoundException {
        if (!shown(id)) throw new FileNotFoundException(id);
        return id.equals(TOP) ? base() : new File(base(), id);
    }

    private static String childId(String parentId, String name) {
        return parentId.equals(TOP) ? name : parentId + "/" + name;
    }

    private static String parentId(String id) {
        int cut = id.lastIndexOf('/');
        return cut < 0 ? TOP : id.substring(0, cut);
    }

    private static String mime(File f) {
        if (f.isDirectory()) return Document.MIME_TYPE_DIR;
        String n = f.getName();
        if (n.endsWith(".png")) return "image/png";
        if (n.endsWith(".txt")) return "text/plain";
        return "application/octet-stream";
    }

    private void addRow(MatrixCursor c, String id, File f) {
        int flags;
        if (f.isDirectory()) {
            flags = Document.FLAG_DIR_SUPPORTS_CREATE;
        } else {
            flags = Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE
                  | Document.FLAG_SUPPORTS_RENAME;
        }
        c.newRow()
            .add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_MIME_TYPE, mime(f))
            .add(Document.COLUMN_DISPLAY_NAME, id.equals(TOP) ? "Knight of Lodis" : f.getName())
            .add(Document.COLUMN_LAST_MODIFIED, f.lastModified())
            .add(Document.COLUMN_FLAGS, flags)
            .add(Document.COLUMN_SIZE, f.isDirectory() ? null : f.length());
    }

    private void changed(String parentId) {
        getContext().getContentResolver().notifyChange(
                DocumentsContract.buildChildDocumentsUri(authority(), parentId), null);
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLS);
        c.newRow()
            .add(Root.COLUMN_ROOT_ID, ROOT_ID)
            .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_LOCAL_ONLY
                                  | Root.FLAG_SUPPORTS_IS_CHILD)
            .add(Root.COLUMN_ICON, R.mipmap.ic_launcher)
            .add(Root.COLUMN_TITLE, "Knight of Lodis")
            .add(Root.COLUMN_SUMMARY, "Saves and save states")
            .add(Root.COLUMN_DOCUMENT_ID, TOP);
        return c;
    }

    @Override
    public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLS);
        File f = file(id);
        if (!f.exists()) throw new FileNotFoundException(id);
        addRow(c, id, f);
        return c;
    }

    @Override
    public Cursor queryChildDocuments(String parentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLS);
        File[] children = file(parentId).listFiles();
        if (children != null) {
            for (File f : children) {
                String id = childId(parentId, f.getName());
                if (!shown(id)) continue;
                // At the top only files, and the states folders.
                if (parentId.equals(TOP) && f.isDirectory() != statesDir(f.getName())) continue;
                addRow(c, id, f);
            }
        }
        c.setNotificationUri(getContext().getContentResolver(),
                DocumentsContract.buildChildDocumentsUri(authority(), parentId));
        return c;
    }

    @Override
    public boolean isChildDocument(String parentId, String id) {
        return shown(id) && (parentId.equals(TOP) || id.startsWith(parentId + "/"));
    }

    @Override
    public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        File f = file(id);
        if (f.isDirectory()) throw new FileNotFoundException(id);
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String createDocument(String parentId, String mimeType, String name)
            throws FileNotFoundException {
        File parent = file(parentId);
        String id = childId(parentId, name);
        if (!parent.isDirectory() || !nameAllowed(parentId, name)
                || Document.MIME_TYPE_DIR.equals(mimeType))
            throw new FileNotFoundException("not a save or a state: " + name);
        File f = new File(parent, name);
        if (f.isDirectory()) throw new FileNotFoundException(id);
        // Replacing: what was there stays as "<name>.bak" (one, the latest).
        if (f.exists() && !name.endsWith(BAK)) {
            File bak = new File(parent, name + BAK);
            bak.delete();
            if (!f.renameTo(bak)) throw new FileNotFoundException("cannot keep " + name);
        }
        try {
            if (!f.exists() && !f.createNewFile()) throw new FileNotFoundException(id);
        } catch (IOException e) {
            throw new FileNotFoundException(id + ": " + e.getMessage());
        }
        changed(parentId);
        return id;
    }

    @Override
    public String renameDocument(String id, String name) throws FileNotFoundException {
        File from = file(id);
        String toId = childId(parentId(id), name);
        if (id.equals(TOP) || from.isDirectory() || !nameAllowed(parentId(id), name))
            throw new FileNotFoundException("not a save or a state: " + name);
        File to = new File(from.getParentFile(), name);
        if (to.exists()) throw new FileNotFoundException(name + " exists");
        if (!from.renameTo(to)) throw new FileNotFoundException(id);
        changed(parentId(id));
        return toId;
    }

    @Override
    public void deleteDocument(String id) throws FileNotFoundException {
        File f = file(id);
        if (id.equals(TOP) || f.isDirectory() || !f.delete()) throw new FileNotFoundException(id);
        changed(parentId(id));
    }

    @Override
    public String getDocumentType(String id) throws FileNotFoundException {
        return mime(file(id));
    }
}
