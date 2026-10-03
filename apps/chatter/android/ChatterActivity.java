// The one Java class chatter ships. tools/package-android.ps1 javac's + d8's
// it into classes.dex (the manifest sets hasCode="true" for this); past it
// everything still lives in libchatter.so via the android.app.lib_name
// meta-data, which super.onCreate hands to native_app_glue untouched.
//
// Why a shim at all: Wi-Fi chips filter multicast/broadcast frames in
// hardware until some app holds a WifiManager.MulticastLock - without it
// the discovery beats (UDP broadcast on 27474) never reach ws::net's
// browser, so a phone past its freshly-foregrounded minutes simply cannot
// hear the PC. Acquire on create, release on destroy; the lock is
// system-wide for the app, the native side needs no changes.
package com.ws.chatter;

import android.app.NativeActivity;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;

public class ChatterActivity extends NativeActivity {
    private WifiManager.MulticastLock multicastLock;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState); // dlopens libchatter.so, runs the glue
        WifiManager wifi = (WifiManager) getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        multicastLock = wifi.createMulticastLock("chatter_discovery");
        multicastLock.setReferenceCounted(true);
        // Forces the Wi-Fi chip to pass broadcast packets to the kernel.
        multicastLock.acquire();
    }

    @Override
    public void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        super.onDestroy();
    }
}
