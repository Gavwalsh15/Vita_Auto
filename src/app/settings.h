#pragma once

#include "transport/wireless_config.h"

namespace app {

// How the phone is found. Auto listens for a USB cable and the Wi-Fi
// Headunit Server at the same time and uses whichever shows up first.
enum class ConnectionMode { Auto, Usb, Wireless };

inline const char* connectionModeName(ConnectionMode mode)
{
    switch (mode) {
    case ConnectionMode::Usb: return "usb";
    case ConnectionMode::Wireless: return "wifi";
    case ConnectionMode::Auto: break;
    }
    return "auto";
}

// Everything the user can change, kept in one file (config::SETTINGS_PATH).
struct Settings {
    bool audioOnVita = true;
    ConnectionMode connection = ConnectionMode::Auto;
    char host[40] = {};
    WirelessConfig wireless;
    bool carplay = false;
};

Settings loadSettings();
bool saveSettings(const Settings& settings);

} // namespace app
