#pragma once

#include <cstdint>

#include "app/app_errors.h"
#include "video/video_pipeline.h"

namespace app {

// The video-side checks every session loop makes, whichever protocol feeds it.
struct SessionWatch {
    // No first picture within startTimeoutUs of startedAtUs is an error; 0 disables that.
    uint64_t startedAtUs = 0;
    uint64_t startTimeoutUs = 0;
    AppError timeoutError = AppError::None;

    struct Verdict {
        AppError error = AppError::None;
        int detail = 0;
    };

    // The error that should end the session, or AppError::None to carry on.
    Verdict check(uint64_t nowUs) const
    {
        const video::pipeline::Failure failure = video::pipeline::failure();
        if (failure.error != AppError::None)
            return {failure.error, failure.detail};
        if (startTimeoutUs != 0 && !video::pipeline::hasPresentedFrame() &&
            nowUs - startedAtUs >= startTimeoutUs)
            return {timeoutError, 0};
        return {};
    }
};

} // namespace app
