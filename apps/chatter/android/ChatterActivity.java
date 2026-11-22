// The one Java class chatter ships. The native app owns the UI and
// networking; this shim holds the Wi-Fi multicast lock only while the
// activity is visible so discovery broadcasts reach ws::net without
// keeping the radio awake after the app is backgrounded.
package com.ws.chatter;

import android.app.NativeActivity;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

public class ChatterActivity extends NativeActivity {
    private WifiManager.MulticastLock multicastLock;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WifiManager wifi = (WifiManager) getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("chatter_discovery");
            multicastLock.setReferenceCounted(false);
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (multicastLock != null && !multicastLock.isHeld()) {
            multicastLock.acquire();
        }
    }

    @Override
    protected void onStop() {
        releaseMulticastLock();
        super.onStop();
    }

    @Override
    public void onDestroy() {
        releaseMulticastLock();
        super.onDestroy();
    }


    // On recent Android releases NativeActivity's private
    // NativeContentView is not the view served by InputMethodManager.
    // Passing it to showSoftInput is rejected ("view is not served").
    // The decor view is the served view, so use it explicitly. Keep the
    // call on the activity thread; IME transitions can also race animation.
    public void showChatKeyboard() {
        runOnUiThread(() -> {
            final View decor = getWindow().getDecorView();
            decor.setFocusableInTouchMode(true);
            decor.requestFocus();
            final InputMethodManager imm = (InputMethodManager)
                    getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                decor.post(() -> {
                    try {
                        imm.showSoftInput(decor,
                                InputMethodManager.SHOW_IMPLICIT);
                    } catch (RuntimeException ignored) {
                        // IME can reject a request while its window animates.
                    }
                });
            }
        });
    }

    public void hideChatKeyboard() {
        runOnUiThread(() -> {
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
        });
    }

    private void releaseMulticastLock() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
    }
}
