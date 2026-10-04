#include "app/audio_settings.h"
#include "app/config.h"
#include "logger/vita_log.h"

#include <psp2/io/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace app {
namespace {
constexpr const char* LEGACY_AUDIO_SETTING_PATH = "ux0:data/vita_auto_audio.txt";
}

bool loadAudioOnVita()
{
    FILE* file = std::fopen(config::AUDIO_SETTING_PATH, "r");
    const bool legacy = file == nullptr && errno == ENOENT;
    if (legacy)
        file = std::fopen(LEGACY_AUDIO_SETTING_PATH, "r");
    if (file == nullptr)
        return true;
    char value[16]{};
    const bool read = std::fgets(value, sizeof(value), file) != nullptr;
    std::fclose(file);
    const bool enabled = !read || std::strcmp(value, "phone\n") != 0;
    if (legacy && read && saveAudioOnVita(enabled))
        std::remove(LEGACY_AUDIO_SETTING_PATH);
    return enabled;
}

bool saveAudioOnVita(bool enabled)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(config::DATA_DIR, 0777);
    FILE* file = std::fopen(config::AUDIO_SETTING_PATH, "w");
    if (file == nullptr) {
        vitaLog("audio setting: could not open settings file");
        return false;
    }
    const bool written = std::fputs(enabled ? "vita\n" : "phone\n", file) >= 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
        vitaLog("audio setting: could not save settings file");
    return written && closed;
}

} // namespace app
