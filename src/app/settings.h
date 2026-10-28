#pragma once

namespace app {

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

struct Settings {
    bool audioOnVita = true;
    ConnectionMode connection = ConnectionMode::Auto;
    char host[40] = {};
};

Settings loadSettings();
bool saveSettings(const Settings& settings);

} // namespace app
