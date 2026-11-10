#pragma once

#include <cstdint>
#include <cstdio>

inline void formatMac(char* out, const uint8_t mac[6], bool upper)
{
    std::snprintf(out, 18, upper ? "%02X:%02X:%02X:%02X:%02X:%02X" : "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

struct WifiNetInfo {
    char ip[16] = {};
    char ssid[33] = {};
    char bssid[18] = {};

    void setBssid(const uint8_t* mac) { formatMac(bssid, mac, false); }
};

struct WirelessConfig {
    bool bluetooth = true; // False: Bluetooth start is off
    char bluetoothName[32] = "Vita Auto";
    char recoverDeviceName[64] = {};
    // Password of the Wi-Fi network the Vita is on, sent to the phone.
    char wifiPassword[64] = {};
    bool wifiAp = false;
    char wifiApSsid[33] = "Vita Auto";
    char wifiApPassword[64] = "vitaauto";
    int wifiApChannel = 6;
};
