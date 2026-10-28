#include "app/settings.h"
#include "app/config.h"
#include "logger/vita_log.h"

#include <psp2/io/stat.h>
#include <cstdio>
#include <cstring>

namespace app {
namespace {

void applyValue(Settings& settings, const char* key, const char* value)
{
    if (std::strcmp(key, "audio") == 0) {
        settings.audioOnVita = std::strcmp(value, "phone") != 0;
    } else if (std::strcmp(key, "connection") == 0) {
        settings.connection = std::strcmp(value, "usb") == 0 ? ConnectionMode::Usb
            : std::strcmp(value, "wifi") == 0 ? ConnectionMode::Wireless
            : ConnectionMode::Auto;
    } else if (std::strcmp(key, "host") == 0) {
        std::snprintf(settings.host, sizeof(settings.host), "%s", value);
    }
}

} // namespace

Settings loadSettings()
{
    Settings settings;
    FILE* file = std::fopen(config::SETTINGS_PATH, "r");
    if (file == nullptr)
        return settings;
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

    const bool written = std::fprintf(file,
        "Vita Auto settings. Edit and save, then reopen the app.\n"
        "audio: vita or phone (music and navigation)\n"
        "connection: auto, usb or wifi\n"
        "host: phone IP for the Headunit Server when it is not the hotspot gateway\n"
        "audio=%s\nconnection=%s\nhost=%s\n",
        settings.audioOnVita ? "vita" : "phone",
        connectionModeName(settings.connection),
        settings.host) >= 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
        vitaLog("settings: could not save settings file");
    return written && closed;
}

} // namespace app
