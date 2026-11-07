// The circsim editor's one Java class. tools/package-android.ps1 javac's
// + d8's it into classes.dex (the manifest sets hasCode="true" for this);
// past it everything still lives in libcircsim_editor.so through the
// android.app.lib_name meta-data, which NativeActivity hands to
// native_app_glue untouched.
//
// Why a shim at all: the Storage Access Framework's pickers are
// activities, and their answers arrive as onActivityResult - a call the
// framework makes on the ACTIVITY, which pure native code never sees.
// The shim owns that one hop: launchPicker fires the intent from the UI
// thread (startActivityForResult is safe from any thread, but the pick
// should open over a paused, settled activity), and onActivityResult
// forwards the pick to the native SAF bridge (registered by name at
// startup - no exported-symbol mangling). The pick's grant is made
// persistable on the spot, so a schematic saved to the user's chosen
// place stays writable across launches - the restored session's Ctrl+S
// must still reach the same document next week.
package com.ws.circsim_editor;

import android.app.NativeActivity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

public class CircsimActivity extends NativeActivity {
    // The request codes the native bridge's run_pick pairs with (the
    // native side spells the same pair - keep them in step).
    private static final int REQ_OPEN = 1;
    private static final int REQ_SAVE = 2;

    // The native bridge's entry point. Called on the activity's thread
    // with the pick's answer; the native side registered this by name
    // against this class at startup (the SAF bridge's init).
    private native void nativePickerResult(int code, boolean ok, String uri,
                                           String name);

    // Fires the SAF picker. `code` is REQ_OPEN/REQ_SAVE, `save` picks
    // ACTION_CREATE_DOCUMENT (the name-a-new-document picker with the
    // suggested title) over ACTION_OPEN_DOCUMENT, and the whole hop
    // runs on the UI thread so the picker opens over a settled
    // activity. Blocking this call would stall the Java thread - it
    // only posts; the native worker blocks on its own wait instead.
    public void launchPicker(final int code, final boolean save,
                             final String suggested) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent intent = save
                        ? new Intent(Intent.ACTION_CREATE_DOCUMENT)
                        : new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                // .csim has no registered MIME type; every document
                // shows, and the app's own extension gate refuses a
                // wrong pick honestly.
                intent.setType("*/*");
                if (save && suggested != null) {
                    intent.putExtra(Intent.EXTRA_TITLE, suggested);
                }
                startActivityForResult(intent, code);
            }
        });
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode,
                                 Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        final boolean ok = resultCode == RESULT_OK;
        Uri uri = (data != null) ? data.getData() : null;
        String name = null;
        if (ok && uri != null) {
            // Persist the grant: the app must still write this document
            // after the reboot. Some providers never offer a persistable
            // grant - the catch keeps the save working until the reboot,
            // the honest degrade the native side toasts about.
            try {
                getContentResolver().takePersistableUriPermission(uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (SecurityException e) {
                // not persistable: the grant lives until the device
                // restarts, and a re-pick renews it
            }
            name = displayName(uri);
        }
        nativePickerResult(requestCode, ok, uri != null ? uri.toString() : null,
                name);
    }

    // The picked document's display name (the provider's own answer),
    // null when the provider does not offer one - the native side falls
    // back to its own default then.
    private String displayName(Uri uri) {
        try (Cursor cursor =
                     getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                final int at =
                        cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (at >= 0 && !cursor.isNull(at)) {
                    return cursor.getString(at);
                }
            }
        } catch (Exception ignored) {
            // a provider that refuses the query: the fallback name
        }
        return null;
    }
}
