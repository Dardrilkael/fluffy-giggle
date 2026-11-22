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


    // NativeActivity's NDK showSoftInput call does not give Chatter a
    // reliable UI-thread/focus hand-off on every device. Keep the IME
    // request on Android's activity thread and target the currently
    // focused NativeActivity content view (the view with the native
    // input connection), not an unrelated overlay.
    public void showChatKeyboard() {
        runOnUiThread(() -> {
            View target = getWindow().getCurrentFocus();
            if (target == null) {
                target = getWindow().getDecorView().findFocus();
            }
            if (target == null) {
                target = getWindow().getDecorView();
            }
            target.setFocusableInTouchMode(true);
            target.requestFocus();
            final View inputTarget = target;
            inputTarget.post(() -> {
                InputMethodManager imm = (InputMethodManager)
                        getSystemService(INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(inputTarget,
                            InputMethodManager.SHOW_IMPLICIT);
                }
            });
        });
    }

    public void hideChatKeyboard() {
        runOnUiThread(() -> {
            View target = getWindow().getCurrentFocus();
            if (target == null) {
                target = getWindow().getDecorView();
            }
            InputMethodManager imm = (InputMethodManager)
                    getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null && target.getWindowToken() != null) {
                imm.hideSoftInputFromWindow(target.getWindowToken(),
                        InputMethodManager.HIDE_NOT_ALWAYS);
            }
        });
    }

    private void releaseMulticastLock() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
    }
}
