package com.koldecomp.game;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;

/**
 * Options > Technical > Sync Folder > Choose / Change (Mods.syncPick): the
 * system's folder picker, opened from an invisible activity (translucent in
 * the manifest, no view), so the game stays behind it and comes back as soon
 * as a folder is picked; SetupActivity's screen is never shown (asked
 * 2026-11-24). The game then shows its busy box while SyncFolder looks in the
 * folder on a thread.
 */
public class FolderPickActivity extends Activity {
    private static final int REQ = 1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) return;   // the picker is already up
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        // Kept across restarts (SyncFolder takes the permission).
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                   | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        // Opens in Documents, where "Use this folder" is allowed: Android 11
        // and later refuse the storage's top and Download itself.
        if (android.os.Build.VERSION.SDK_INT >= 26) {
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                    "com.android.externalstorage.documents", "primary:Documents"));
        }
        try {
            startActivityForResult(i, REQ);
        } catch (Exception e) {
            Mods.nativeSyncDone("This device has no folder picker.", false);
            finish();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        final Uri tree = result == RESULT_OK && data != null ? data.getData() : null;
        if (tree != null) {
            Mods.nativeSyncBusy("Looking in the folder...");
            final android.content.Context app = getApplicationContext();
            new Thread(() -> SyncFolder.picked(app, tree), "sync-pick").start();
        }
        finish();
        overridePendingTransition(0, 0);
    }
}
