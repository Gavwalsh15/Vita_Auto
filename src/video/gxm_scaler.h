#pragma once

#include <cstdint>

#include "video/vita_h264_decoder.h"

namespace video {

struct GxmFrameTiming {
    uint32_t waitUs;
    uint32_t uploadUs;
    uint32_t drawUs;
    uint32_t totalUs;
};

bool initGxmScaler();
void shutdownGxmScaler();

bool isGxmScalerReady();

// Presents directly through the GXM/vita2d display queue.  When this returns
// true the caller must not publish its CPU framebuffer for the same frame.
bool presentFrameWithGpu(const DecodedFrame& frame);
GxmFrameTiming lastGxmFrameTiming();

} // namespace video
