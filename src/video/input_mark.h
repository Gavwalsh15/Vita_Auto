#pragma once

#include "inputs/vita_input.h"
#include "logger/performance.h"
#include "video/video_pipeline.h"

namespace video {

// Brackets a stretch of a session loop that sends queued input: if any was
// delivered, the video pipeline times how long the next picture takes.
class InputMark {
public:
    void begin() { delivered_ = inputs::deliveryStats().delivered; }
    void end() const
    {
        if (inputs::deliveryStats().delivered != delivered_)
            pipeline::noteInputSubmitted(logger::performance::nowUs());
    }

private:
    uint32_t delivered_ = 0;
};

} // namespace video
