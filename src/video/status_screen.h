#pragma once

namespace video {

// Draw through libvita2d's double-buffered display path using the Vita default
// PVF font. The video presenter owns GXM/vita2d initialization.
void drawStatusScreen(
    const char* title,
    const char* detail,
    const char* error,
    const char* action,
    const char* audio
);
void shutdownStatusScreen();

} // namespace video
