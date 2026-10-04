#pragma once

#include <cstddef>
#include <cstdint>

namespace video {

struct H264AccessUnitInfo {
    uint32_t accessUnitCount = 0;
    uint32_t nalCount = 0;
    uint32_t bytes = 0;
    uint32_t idrCount = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t profile = 0;
    uint32_t pictureOrderType = 0;
    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;
    bool hasSlice = false;
    bool parsedSps = false;
};

class H264AnnexBParser {
public:
    H264AccessUnitInfo inspectAccessUnit(const uint8_t* data, size_t size);

private:
    uint32_t accessUnitCount_ = 0;
    uint32_t activeWidth_ = 0;
    uint32_t activeHeight_ = 0;
};

} // namespace video
