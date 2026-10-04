#pragma once

namespace app {
namespace config {

constexpr const char* DATA_DIR = "ux0:data/vita_auto";
constexpr const char* LOG_PATH = "ux0:data/vita_auto/vita_auto.log";
constexpr const char* AUDIO_SETTING_PATH = "ux0:data/vita_auto/vita_auto_audio.txt";

constexpr int SCREEN_WIDTH = 960;
constexpr int SCREEN_HEIGHT = 544;
constexpr int SCREEN_PITCH = 960;

constexpr unsigned IDLE_POLL_US = 250;

} // namespace config
} // namespace app
