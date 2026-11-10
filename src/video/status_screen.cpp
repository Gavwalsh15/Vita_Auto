#include "video/status_screen.h"
#include "app/config.h"
#include "logger/vita_log.h"

#include <vita2d.h>

#include <cstdio>

namespace video {
namespace {

constexpr unsigned int BACKGROUND_COLOR = 0xff231a14u;
constexpr unsigned int ACCENT_COLOR = 0xffb6d74au;
constexpr unsigned int PRIMARY_TEXT_COLOR = 0xfff6efeau;
constexpr unsigned int SECONDARY_TEXT_COLOR = 0xffa99c90u;
// The accent colour, dimmed, for the credit beside the title.
constexpr unsigned int CREDIT_COLOR = 0xff768c30u;

vita2d_pvf* g_statusFont = nullptr;

vita2d_pvf* statusFont()
{
    if (g_statusFont == nullptr) {
        // PVF pads cached glyphs so enlarged text cannot sample its neighbours.
        g_statusFont = vita2d_load_default_pvf();
        if (g_statusFont == nullptr)
            vitaLog("status screen: default PVF font load failed");
    }
    return g_statusFont;
}

void drawText(int x, int y, float scale, unsigned int color, const char* value)
{
    vita2d_pvf* const font = statusFont();
    if (font == nullptr || value == nullptr)
        return;

    vita2d_pvf_draw_text(font, x, y, color, scale, value);
}

// Left edge of every row of text, and where the choices of a row start.
constexpr int MARGIN_X = 52;
constexpr int CHOICE_X = 270;

void drawChoiceRow(int y, const ChoiceRow& row)
{
    vita2d_pvf* const font = statusFont();
    if (font == nullptr)
        return;

    drawText(MARGIN_X, y, 1.1f, SECONDARY_TEXT_COLOR, row.name);
    int x = CHOICE_X;
    char label[40];
    for (int i = 0; i < row.count; ++i) {
        const bool active = i == row.selected;
        std::snprintf(label, sizeof(label), active ? "[ %s ]" : "%s", row.choices[i]);
        drawText(x, y, 1.1f, active ? ACCENT_COLOR : SECONDARY_TEXT_COLOR, label);
        x += vita2d_pvf_text_width(font, 1.1f, label) + 24;
    }
    if (row.hint != nullptr && row.hint[0] != '\0')
        drawText(x + 16, y, 0.9f, CREDIT_COLOR, row.hint);
}

} // namespace

void drawStatusScreen(
    const char* title,
    const char* detail,
    const char* note,
    const char* action,
    const ChoiceRow* rows,
    int rowCount
) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(
        0.0f,
        0.0f,
        static_cast<float>(app::config::SCREEN_WIDTH),
        static_cast<float>(app::config::SCREEN_HEIGHT),
        BACKGROUND_COLOR
    );

    // The product name and credit lead; the state of the connection is smaller.
    drawText(MARGIN_X, 92, 2.6f, ACCENT_COLOR, "VITA AUTO");
    if (vita2d_pvf* const font = statusFont()) {
        const int nameWidth = vita2d_pvf_text_width(font, 2.6f, "VITA AUTO");
        drawText(MARGIN_X + nameWidth + 24, 92, 1.4f, CREDIT_COLOR, "BY GAVWALSH15");
    }
    vita2d_draw_rectangle(static_cast<float>(MARGIN_X), 128.0f, 68.0f, 4.0f, ACCENT_COLOR);
    drawText(MARGIN_X, 192, 1.5f, PRIMARY_TEXT_COLOR, title);
    drawText(MARGIN_X, 236, 1.2f, PRIMARY_TEXT_COLOR, detail);
    drawText(MARGIN_X, 276, 1.0f, SECONDARY_TEXT_COLOR, note);
    int y = 336;
    for (int i = 0; i < rowCount; ++i, y += 40)
        drawChoiceRow(y, rows[i]);
    drawText(MARGIN_X, 444, 1.0f, ACCENT_COLOR, action);
    drawText(MARGIN_X, 502, 1.0f, SECONDARY_TEXT_COLOR, app::config::LOG_PATH);

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void shutdownStatusScreen()
{
    if (g_statusFont != nullptr) {
        // The last status scene may still be sampling the font atlas.
        vita2d_wait_rendering_done();
        vita2d_free_pvf(g_statusFont);
        g_statusFont = nullptr;
    }
}

} // namespace video
