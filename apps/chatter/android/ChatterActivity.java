// The one Java class chatter ships. The native app owns the UI and
// networking; this shim holds the Wi-Fi multicast lock only while the
// activity is visible so discovery broadcasts reach ws::net without
// keeping the radio awake after the app is backgrounded.
package com.ws.chatter;

import android.app.NativeActivity;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;

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

    private void releaseMulticastLock() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
    }
}
