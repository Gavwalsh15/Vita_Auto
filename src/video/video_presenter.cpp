#include "video/video_presenter.h"
#include "video/status_screen.h"
#include "logger/vita_log.h"
#include "app/config.h"
#include "video/gxm_scaler.h"
#include "video/vita_h264_decoder.h"

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <cstring>

namespace video {
namespace presenter {
namespace {

constexpr uint32_t DISPLAY_BUFFER_COUNT = 3;
alignas(4096) static uint32_t
    g_framebuffers[DISPLAY_BUFFER_COUNT][app::config::SCREEN_PITCH * app::config::SCREEN_HEIGHT];
static uint32_t g_frontFramebufferIndex = 0;
uint32_t acquireBackFramebufferIndex()
{
    const uint32_t front = __atomic_load_n(
        &g_frontFramebufferIndex,
        __ATOMIC_ACQUIRE
    );
    return (front + 1) % DISPLAY_BUFFER_COUNT;
}

bool publishFramebuffer(uint32_t bufferIndex)
{
    if (bufferIndex >= DISPLAY_BUFFER_COUNT)
        return false;

    SceDisplayFrameBuf framebuf{};
    framebuf.size = sizeof(framebuf);
    framebuf.base = g_framebuffers[bufferIndex];
    framebuf.pitch = app::config::SCREEN_PITCH;
    framebuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    framebuf.width = app::config::SCREEN_WIDTH;
    framebuf.height = app::config::SCREEN_HEIGHT;

    sceDisplayWaitVblankStart();
    const int result = sceDisplaySetFrameBuf(&framebuf, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (result < 0) {
        vitaLogAppError(app::AppError::DisplayPresentFailed, result);
        return false;
    }

    // Publish the queued scanout surface before choosing the next back buffer.
    __atomic_store_n(
        &g_frontFramebufferIndex,
        bufferIndex,
        __ATOMIC_RELEASE
    );
    return true;
}

bool presentDecodedFrame(const video::DecodedFrame& frame)
{
    if (frame.pixels == nullptr || frame.width == 0 || frame.height == 0)
        return false;

    const uint32_t sourcePitchBytes = frame.pitchBytes();
    const uint8_t* source = frame.pixels;
    const uint32_t backIndex = acquireBackFramebufferIndex();
    uint32_t* const backBuffer = g_framebuffers[backIndex];

    if (frame.width == app::config::SCREEN_WIDTH && frame.height == app::config::SCREEN_HEIGHT)
    {
        // AVCDEC RGBA8888 bytes are directly compatible with the Vita
        // A8B8G8R8 scanout layout. Copy complete rows into the back surface.
        for (int y = 0; y < app::config::SCREEN_HEIGHT; ++y)
        {
            std::memcpy(
                backBuffer + (y * app::config::SCREEN_PITCH),
                source + (static_cast<uint32_t>(y) * sourcePitchBytes),
                app::config::SCREEN_WIDTH * sizeof(uint32_t)
            );
        }
    }
    else
    {
        for (int y = 0; y < app::config::SCREEN_HEIGHT; ++y)
        {
            const uint32_t sy = static_cast<uint32_t>(
                static_cast<uint64_t>(y) * frame.height / app::config::SCREEN_HEIGHT
            );
            uint32_t* out = backBuffer + (y * app::config::SCREEN_PITCH);

            for (int x = 0; x < app::config::SCREEN_WIDTH; ++x)
            {
                const uint32_t sx = static_cast<uint32_t>(
                    static_cast<uint64_t>(x) * frame.width / app::config::SCREEN_WIDTH
                );
                const uint8_t* p = source + (sy * sourcePitchBytes) + (sx * 4);
                out[x] = 0xFF000000u |
                    (static_cast<uint32_t>(p[2]) << 16) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    static_cast<uint32_t>(p[0]);
            }
        }
    }

    return publishFramebuffer(backIndex);
}

} // namespace

void init()
{
    video::initGxmScaler();
}

bool shutdown()
{
    if (video::isGxmScalerReady()) {
        video::shutdownStatusScreen();
        // Finish old display callbacks before selecting a persistent CPU buffer.
        // libvita2d_fini frees its scanout buffers; the display must have stopped
        // using them, not merely have a replacement queued for the next vblank.
        const int queueResult = sceGxmDisplayQueueFinish();
        if (queueResult < 0) {
            vitaLogAppError(app::AppError::GraphicsShutdownFailed, queueResult);
            return false;
        }
        const uint32_t backIndex = acquireBackFramebufferIndex();
        std::memset(g_framebuffers[backIndex], 0, sizeof(g_framebuffers[backIndex]));
        if (!publishFramebuffer(backIndex))
            return false;
        const int waitResult = sceDisplayWaitSetFrameBuf();
        if (waitResult < 0) {
            vitaLogAppError(app::AppError::GraphicsShutdownFailed, waitResult);
            return false;
        }
    }
    video::shutdownGxmScaler();
    return true;
}

bool presentCpuFallback(const video::DecodedFrame& frame)
{
    return presentDecodedFrame(frame);
}

void presentStatus(const char* title, const char* detail, const char* error,
                   const char* action, const char* audio)
{
    if (!video::isGxmScalerReady())
        init();
    video::drawStatusScreen(title, detail, error, action, audio);
}

} // namespace presenter
} // namespace video
