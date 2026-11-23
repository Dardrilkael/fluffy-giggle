// Chatter-specific Android lifecycle integration. Keyboard handling is
// inherited from the shared ws::ui Activity base.
package com.ws.chatter;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;

public class ChatterActivity extends com.ws.ui.WsUiActivity {
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
