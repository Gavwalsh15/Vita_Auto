#include "video/status_screen.h"
#include "app/config.h"
#include "logger/vita_log.h"

#include <vita2d.h>

namespace video {
namespace {

constexpr unsigned int BACKGROUND_COLOR = 0xff231a14u;
constexpr unsigned int ACCENT_COLOR = 0xffb6d74au;
constexpr unsigned int PRIMARY_TEXT_COLOR = 0xfff6efeau;
constexpr unsigned int SECONDARY_TEXT_COLOR = 0xffa99c90u;

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

} // namespace

void drawStatusScreen(
    const char* title,
    const char* detail,
    const char* error,
    const char* action,
    const char* audio
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

    drawText(52, 76, 1.2f, ACCENT_COLOR, "VITA AUTO");
    vita2d_draw_rectangle(52.0f, 110.0f, 68.0f, 4.0f, ACCENT_COLOR);
    drawText(52, 188, 2.8f, PRIMARY_TEXT_COLOR, title);
    drawText(52, 252, 1.5f, PRIMARY_TEXT_COLOR, detail);
    drawText(52, 320, 1.0f, SECONDARY_TEXT_COLOR, error);
    drawText(52, 380, 1.0f, SECONDARY_TEXT_COLOR, audio);
    drawText(52, 430, 1.0f, ACCENT_COLOR, action);
    drawText(52, 502, 1.0f, SECONDARY_TEXT_COLOR, app::config::LOG_PATH);

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
