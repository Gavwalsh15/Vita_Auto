#pragma once

#include <cstdint>
#include "app/app_errors.h"

namespace android_auto { struct MediaFrame; }

namespace video {
namespace pipeline {

struct Failure {
    app::AppError error = app::AppError::None;
    int detail = 0;
};
Failure failure();
bool hasPresentedFrame();

// IO thread copies media into an ordered queue, decoder work starts lazily.
void acceptFrame(const android_auto::MediaFrame& frame,
                 uint32_t& sequence, uint32_t& frames, const char* logPrefix);
void noteInputSubmitted(uint64_t submittedAtUs);
uint32_t peakQueueDepth(); // IO-thread diagnostic snapshot.

// Main thread uploads the latest picture under the decoder's output mutex.
// Returns true when a new frame was displayed. CPU fallback also runs here.
bool present();
// Call after IO has joined, while the display resources are still alive.
void stop();

} // namespace pipeline
} // namespace video
