#pragma once

#include <cstddef>
#include <cstdint>
#include "app/app_errors.h"

namespace video {
namespace pipeline {

// One H.264 access unit in Annex B form, from whichever protocol delivered it. The
// rx and parse times are 0 when the source does not measure them.
struct Frame {
    uint64_t timestamp = 0; // The sender's clock, microseconds; 0 when it sends none
    uint64_t rxFirstCompletedAtUs = 0;
    uint64_t rxLastCompletedAtUs = 0;
    uint64_t parsedAtUs = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct Failure {
    app::AppError error = app::AppError::None;
    int detail = 0;
};
Failure failure();
bool hasPresentedFrame();

// IO thread copies media into an ordered queue, decoder work starts lazily.
void acceptFrame(const Frame& frame,
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
