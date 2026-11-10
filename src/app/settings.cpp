#include "app/settings.h"
#include "app/config.h"
#include "logger/vita_log.h"

#include <psp2/io/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app {
namespace {

// One row per setting: how it is read, how it is written, and what the file says about it.
struct Entry {
    const char* key;
    const char* help;
    void (*load)(Settings&, const char* value);
    void (*store)(const Settings&, char* out, size_t size);
};

void copyText(char* dest, size_t size, const char* value)
{
    std::snprintf(dest, size, "%s", value);
}

const char* onOff(bool on) { return on ? "on" : "off"; }

// The file is written in this order.
const Entry ENTRIES[] = {
    {"audio", "vita or phone (music and navigation)",
     [](Settings& s, const char* v) { s.audioOnVita = std::strcmp(v, "phone") != 0; },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.audioOnVita ? "vita" : "phone"); }},
    {"connection", "auto, usb or wifi",
     [](Settings& s, const char* v) {
         s.connection = std::strcmp(v, "usb") == 0 ? ConnectionMode::Usb
             : std::strcmp(v, "wifi") == 0 ? ConnectionMode::Wireless
             : ConnectionMode::Auto;
     },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, connectionModeName(s.connection)); }},
    {"host", "phone IP for the Headunit Server when it is not the hotspot gateway",
     [](Settings& s, const char* v) { copyText(s.host, sizeof(s.host), v); },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.host); }},
    {"bluetooth", "on or off (Bluetooth wireless start)",
     [](Settings& s, const char* v) { s.wireless.bluetooth = std::strcmp(v, "off") != 0; },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, onOff(s.wireless.bluetooth)); }},
    {"carplay", "on or off (Apple CarPlay instead of Android Auto, see docs/carplay.md)",
     [](Settings& s, const char* v) { s.carplay = std::strcmp(v, "on") == 0; },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, onOff(s.carplay)); }},
    {"carplay_audio", "full, opus or basic",
     [](Settings& s, const char* v) {
         s.carplayAudio = std::strcmp(v, "basic") == 0 ? carplay::AudioOffer::Basic
             : std::strcmp(v, "opus") == 0 ? carplay::AudioOffer::Opus
             : carplay::AudioOffer::Full;
     },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, carplay::audioOfferName(s.carplayAudio)); }},
    {"bluetooth_name", "name shown to phones while the app is open, empty keeps the Vita name",
     [](Settings& s, const char* v) { copyText(s.wireless.bluetoothName, sizeof(s.wireless.bluetoothName), v); },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.wireless.bluetoothName); }},
    {"wifi_password", "password of the Wi-Fi network the Vita is on (sent to the phone)",
     [](Settings& s, const char* v) { copyText(s.wireless.wifiPassword, sizeof(s.wireless.wifiPassword), v); },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.wireless.wifiPassword); }},
    {"wifi_ap", "on or off (experiment: with no Wi-Fi network, the Vita hosts one for the phone)",
     [](Settings& s, const char* v) { s.wireless.wifiAp = std::strcmp(v, "on") == 0; },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, onOff(s.wireless.wifiAp)); }},
    {"wifi_ap_ssid", "name of the network the Vita hosts",
     [](Settings& s, const char* v) {
         if (v[0] != '\0')
             copyText(s.wireless.wifiApSsid, sizeof(s.wireless.wifiApSsid), v);
     },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.wireless.wifiApSsid); }},
    {"wifi_ap_password", "empty for an open network, else 8-63 characters",
     [](Settings& s, const char* v) { copyText(s.wireless.wifiApPassword, sizeof(s.wireless.wifiApPassword), v); },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.wireless.wifiApPassword); }},
    {"wifi_ap_channel", "1-11",
     [](Settings& s, const char* v) {
         const int channel = std::atoi(v);
         if (channel >= 1 && channel <= 11)
             s.wireless.wifiApChannel = channel;
     },
     [](const Settings& s, char* o, size_t n) { std::snprintf(o, n, "%d", s.wireless.wifiApChannel); }},
    {"original_device_name", "set by the app, do not edit",
     [](Settings& s, const char* v) { copyText(s.wireless.recoverDeviceName, sizeof(s.wireless.recoverDeviceName), v); },
     [](const Settings& s, char* o, size_t n) { copyText(o, n, s.wireless.recoverDeviceName); }},
};

void applyValue(Settings& settings, const char* key, const char* value)
{
    for (const Entry& entry : ENTRIES) {
        if (std::strcmp(key, entry.key) == 0) {
            entry.load(settings, value);
            return;
        }
    }
}

bool loadSettingsFile(Settings& settings)
{
    FILE* file = std::fopen(config::SETTINGS_PATH, "r");
    if (file == nullptr)
        return false;
    char line[96];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        line[std::strcspn(line, "\r\n")] = '\0';
        char* equals = std::strchr(line, '=');
        if (equals == nullptr)
            continue;
        *equals = '\0';
        applyValue(settings, line, equals + 1);
    }
    std::fclose(file);
    return true;
}

} // namespace

Settings loadSettings()
{
    Settings settings;
    if (!loadSettingsFile(settings))
        saveSettings(settings); // Missing or unreadable: write the defaults.
    return settings;
}

bool saveSettings(const Settings& settings)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(config::DATA_DIR, 0777);
    FILE* file = std::fopen(config::SETTINGS_PATH, "w");
    if (file == nullptr) {
        vitaLog("settings: could not open settings file");
        return false;
    }
    // Every option is always written, so the file shows what can be changed.
    // Lines without '=' are comments and are ignored when loading.
    bool written = std::fprintf(file, "Vita Auto settings. Edit and save, then reopen the app.\n") >= 0;
    for (const Entry& entry : ENTRIES)
        written = written && std::fprintf(file, "%s: %s\n", entry.key, entry.help) >= 0;
    for (const Entry& entry : ENTRIES) {
        char value[96];
        entry.store(settings, value, sizeof(value));
        written = written && std::fprintf(file, "%s=%s\n", entry.key, value) >= 0;
    }
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
        vitaLog("settings: could not save settings file");
    return written && closed;
}

} // namespace app
