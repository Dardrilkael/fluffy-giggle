// Shared Android Activity base for ws::ui applications.
// NativeActivity's private NativeContentView is not consistently accepted by
// InputMethodManager on recent Android releases. The decor view is the served
// view, so all ws::ui apps use this one UI-thread keyboard bridge.
package com.ws.ui;

import android.app.NativeActivity;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

public class WsUiActivity extends NativeActivity {
    public void showWsUiKeyboard() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                final View decor = getWindow().getDecorView();
                final InputMethodManager imm = (InputMethodManager)
                        getSystemService(INPUT_METHOD_SERVICE);
                if (imm != null) {
                    try {
                        imm.showSoftInput(decor,
                                InputMethodManager.SHOW_IMPLICIT);
                    } catch (RuntimeException ignored) {
                        // IME can reject a request while its window animates.
                    }
                }
            }
        });
    }

    public void hideWsUiKeyboard() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                final View decor = getWindow().getDecorView();
                final InputMethodManager imm = (InputMethodManager)
                        getSystemService(INPUT_METHOD_SERVICE);
                if (imm != null && decor.getWindowToken() != null) {
                    try {
                        imm.hideSoftInputFromWindow(decor.getWindowToken(),
                                InputMethodManager.HIDE_NOT_ALWAYS);
                    } catch (RuntimeException ignored) {
                        // Hiding during an IME transition is already satisfied.
                    }
                }
            }
        });
    }
}
