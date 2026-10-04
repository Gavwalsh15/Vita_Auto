#include "video/gxm_scaler.h"
#include "app/config.h"

#include <algorithm>
#include <cstring>

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include "logger/vita_log.h"
#include "logger/performance.h"

namespace video {
namespace {

constexpr uint32_t TEXTURE_POOL_BYTES = 2 * 1024 * 1024;

bool g_ready = false;
vita2d_texture* g_texture = nullptr;
vita2d_texture g_zeroCopyTexture{};
const uint8_t* g_zeroCopyPixels = nullptr;
bool g_loggedZeroCopy = false;
bool g_loggedZeroCopyFallback = false;
uint32_t g_textureWidth = 0;
uint32_t g_textureHeight = 0;
uint32_t g_presentedFrames = 0;
uint32_t g_failedPresents = 0;
GxmFrameTiming g_lastFrameTiming{};

void destroyTexture()
{
    if (g_texture != nullptr) {
        vita2d_free_texture(g_texture);
        g_texture = nullptr;
    }
    g_textureWidth = 0;
    g_textureHeight = 0;
}

vita2d_texture* prepareZeroCopyTexture(const DecodedFrame& frame)
{
    if (!frame.gpuMapped || frame.pitch != frame.width)
        return nullptr;

    if (g_zeroCopyPixels == frame.pixels)
        return &g_zeroCopyTexture;

    std::memset(&g_zeroCopyTexture, 0, sizeof(g_zeroCopyTexture));
    g_zeroCopyTexture.data_UID = -1;
    g_zeroCopyTexture.palette_UID = -1;
    g_zeroCopyTexture.depth_UID = -1;

    const int result = sceGxmTextureInitLinear(
        &g_zeroCopyTexture.gxm_tex,
        const_cast<uint8_t*>(frame.pixels),
        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,
        frame.width,
        frame.height,
        0
    );
    if (result < 0) {
        if (!g_loggedZeroCopyFallback) {
            vitaLog(
            "[%s] GXM zero-copy texture init failed err=0x%08x; copy fallback active", app::appErrorName(app::AppError::GraphicsTextureBindFailed), result);
            g_loggedZeroCopyFallback = true;
        }
        return nullptr;
    }

    sceGxmTextureSetMinFilter(
        &g_zeroCopyTexture.gxm_tex,
        SCE_GXM_TEXTURE_FILTER_LINEAR
    );
    sceGxmTextureSetMagFilter(
        &g_zeroCopyTexture.gxm_tex,
        SCE_GXM_TEXTURE_FILTER_LINEAR
    );
    g_zeroCopyPixels = frame.pixels;

    if (!g_loggedZeroCopy) {
        vitaLogInfo(
            "GXM zero-copy AVCDEC texture ready size=%ux%u pitch=%u",
            frame.width,
            frame.height,
            frame.pitch
        );
        g_loggedZeroCopy = true;
    }
    return &g_zeroCopyTexture;
}

bool createTexture(uint32_t width, uint32_t height)
{
    destroyTexture();

    // Use uncached main memory first because the decoder writes from the CPU
    // every frame.  libvita2d/gpu_alloc still falls back if this is not
    // available on the current memory layout.
    vita2d_texture_set_alloc_memblock_type(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE
    );

    g_texture = vita2d_create_empty_texture_format(
        width,
        height,
        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8
    );
    if (g_texture == nullptr) {
        ++g_failedPresents;
        vitaLog(
            "[%s] GXM texture create failed size=%ux%u", app::appErrorName(app::AppError::GraphicsTextureCreateFailed), width, height);
        return false;
    }

    vita2d_texture_set_filters(
        g_texture,
        SCE_GXM_TEXTURE_FILTER_LINEAR,
        SCE_GXM_TEXTURE_FILTER_LINEAR
    );

    g_textureWidth = width;
    g_textureHeight = height;
    vitaLogInfo("GXM texture ready size=%ux%u", width, height);
    return true;
}

bool ensureTexture(const DecodedFrame& frame)
{
    if (g_texture != nullptr &&
        g_textureWidth == frame.width &&
        g_textureHeight == frame.height)
    {
        return true;
    }

    return createTexture(frame.width, frame.height);
}

bool uploadFrameToTexture(const DecodedFrame& frame)
{
    if (!ensureTexture(frame))
        return false;

    // The preceding scene may still be sampling this texture after its display
    // swap was queued.  Do not overwrite it until GXM has completed that scene.
    const uint64_t waitStart = logger::performance::nowUs();
    if (g_presentedFrames > 0)
        vita2d_wait_rendering_done();
    g_lastFrameTiming.waitUs = static_cast<uint32_t>(
        logger::performance::nowUs() - waitStart
    );

    uint8_t* const destination =
        static_cast<uint8_t*>(vita2d_texture_get_datap(g_texture));
    if (destination == nullptr)
        return false;

    // libvita2d returns texture stride in bytes, not pixels.
    const uint32_t destinationPitchBytes =
        vita2d_texture_get_stride(g_texture);
    const uint32_t inputPitchBytes = frame.pitchBytes();
    const uint32_t copyBytes = std::min(frame.width * 4, destinationPitchBytes);

    const uint64_t uploadStart = logger::performance::nowUs();
    for (uint32_t y = 0; y < frame.height; ++y)
    {
        std::memcpy(
            destination + (y * destinationPitchBytes),
            frame.pixels + (y * inputPitchBytes),
            copyBytes
        );
    }
    g_lastFrameTiming.uploadUs = static_cast<uint32_t>(
        logger::performance::nowUs() - uploadStart
    );

    return true;
}

} // namespace

bool initGxmScaler()
{
    if (g_ready)
        return true;

    g_presentedFrames = 0;
    g_failedPresents = 0;
    g_loggedZeroCopy = false;
    g_loggedZeroCopyFallback = false;
    g_lastFrameTiming = GxmFrameTiming{};

    const int result = vita2d_init_advanced(TEXTURE_POOL_BYTES);
    if (result <= 0) {
        vitaLog(
            "[%s] vita2d init failed result=0x%08x; CPU scaler fallback active", app::appErrorName(app::AppError::GraphicsInitFailed),
            result
        );
        g_ready = false;
        return false;
    }

    vita2d_set_clear_color(0xFF000000);
    vita2d_set_vblank_wait(1);

    g_ready = true;
    vitaLogInfo(
        "GXM textured scaler ready output=%ux%u pitch=%u pool=%u",
        app::config::SCREEN_WIDTH,
        app::config::SCREEN_HEIGHT,
        app::config::SCREEN_PITCH,
        TEXTURE_POOL_BYTES
    );
    return true;
}

void shutdownGxmScaler()
{
    if (!g_ready)
        return;

    vita2d_wait_rendering_done();
    destroyTexture();
    g_zeroCopyPixels = nullptr;
    std::memset(&g_zeroCopyTexture, 0, sizeof(g_zeroCopyTexture));
    const int result = vita2d_fini();
    if (result < 0)
        vitaLog(
            "[%s] vita2d fini failed err=0x%08x", app::appErrorName(app::AppError::GraphicsShutdownFailed), result);

    g_ready = false;
}

bool isGxmScalerReady()
{
    return g_ready;
}

bool presentFrameWithGpu(const DecodedFrame& frame)
{
    if (!g_ready || frame.pixels == nullptr || frame.width == 0 || frame.height == 0)
        return false;

    g_lastFrameTiming = {};
    const uint64_t totalStart = logger::performance::nowUs();

    vita2d_texture* drawTexture = prepareZeroCopyTexture(frame);
    const bool zeroCopy = drawTexture != nullptr;
    if (!zeroCopy) {
        if (!frame.gpuMapped && !g_loggedZeroCopyFallback) {
            vitaLog("GXM AVCDEC output is not GPU-mapped; copy fallback active");
            g_loggedZeroCopyFallback = true;
        }
        if (!uploadFrameToTexture(frame)) {
            ++g_failedPresents;
            if (g_failedPresents == 1)
                vitaLog(
            "[%s] GXM frame upload failed failures=%u", app::appErrorName(app::AppError::GraphicsUploadFailed), g_failedPresents);
            return false;
        }
        drawTexture = g_texture;
    }

    const float xScale =
        static_cast<float>(app::config::SCREEN_WIDTH) / static_cast<float>(frame.width);
    const float yScale =
        static_cast<float>(app::config::SCREEN_HEIGHT) / static_cast<float>(frame.height);

    const uint64_t drawStart = logger::performance::nowUs();
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_texture_scale(drawTexture, 0.0f, 0.0f, xScale, yScale);
    vita2d_end_drawing();

    // The decoder mutex is held by the caller. In zero-copy mode this fence
    // guarantees that GXM has finished sampling AVCDEC's output before the
    // worker may submit another access unit and let AVCDEC reuse the buffer.
    if (zeroCopy)
        vita2d_wait_rendering_done();

    vita2d_swap_buffers();
    g_lastFrameTiming.drawUs = static_cast<uint32_t>(
        logger::performance::nowUs() - drawStart
    );
    g_lastFrameTiming.totalUs = static_cast<uint32_t>(
        logger::performance::nowUs() - totalStart
    );

    ++g_presentedFrames;
    if (g_presentedFrames == 1)
    {
        vitaLogInfo(
            "GXM presented frame=%u src=%ux%u scale=%.3fx%.3f",
            g_presentedFrames,
            frame.width,
            frame.height,
            static_cast<double>(xScale),
            static_cast<double>(yScale)
        );
    }

    return true;
}

GxmFrameTiming lastGxmFrameTiming()
{
    return g_lastFrameTiming;
}

} // namespace video
