#pragma once

namespace video {

// One setting shown as a row of choices with the active one highlighted.
struct ChoiceRow {
    const char* name;
    const char* const* choices;
    int count;
    int selected;
    const char* hint; // Button prompt shown after the choices; may be empty.
};

// Draw through libvita2d's double-buffered display path using the Vita default
// PVF font. The video presenter owns GXM/vita2d initialization.
void drawStatusScreen(
    const char* title,
    const char* detail,
    const char* note,
    const char* action,
    const ChoiceRow* rows,
    int rowCount
);
void shutdownStatusScreen();

} // namespace video
