#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

struct SpsRewriteInfo {
    uint32_t spsCount = 0;
    uint32_t rewritten = 0;
    uint32_t failed = 0;
    uint32_t skipped = 0;         // pictures might be reordered, so the SPS was left alone
    uint32_t maxNumRefFrames = 0;
    bool hadVui = false;
    bool hadRestriction = false; // the stream already declared its own limits
};

SpsRewriteInfo rewriteSpsForLowLatency(std::vector<uint8_t>& accessUnit);

class H264AnnexBParser {
public:
    H264AccessUnitInfo inspectAccessUnit(const uint8_t* data, size_t size);

private:
    uint32_t accessUnitCount_ = 0;
    uint32_t activeWidth_ = 0;
    uint32_t activeHeight_ = 0;
};

} // namespace video
