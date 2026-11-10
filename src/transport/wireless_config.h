#pragma once

// The wireless settings every wireless path uses: Android Auto's Bluetooth
// bootstrap and CarPlay. Held in app::Settings and passed on as it is.
struct WirelessConfig {
    bool bluetooth = true; // False: Bluetooth start is off
    char bluetoothName[32] = "Vita Auto";
    // The Vita's own device name from a run that did not restore it (or empty).
    char recoverDeviceName[64] = {};
    // Password of the Wi-Fi network the Vita is on, sent to the phone.
    char wifiPassword[64] = {};
    // Experimental: host an access point when the Vita is on no network.
    bool wifiAp = false;
    char wifiApSsid[33] = "Vita Auto";
    char wifiApPassword[64] = "vitaauto";
    int wifiApChannel = 6;
};
