#pragma once

#include <cstddef>
#include <cstdint>

#include <psp2/videodec.h>

namespace video {

struct DecodedFrame {
    // AVCDEC RGBA8888 output; pitch is measured in pixels.
    const uint8_t* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    bool gpuMapped = false;
    uint32_t pitchBytes() const { return pitch * sizeof(uint32_t); }
};

class VitaH264Decoder {
public:
    bool init(uint32_t width, uint32_t height);
    bool submitAccessUnit(const uint8_t* data, size_t size);
    bool hasFrame() const;
    const DecodedFrame& latestFrame() const;
    void shutdown();

    bool initialized() const { return initialized_; }
    uint32_t submittedAccessUnits() const { return submittedAccessUnits_; }
    int lastError() const { return lastError_; }
    uint32_t lastInitStage() const { return lastInitStage_; }
    uint32_t lastOutputCount() const { return lastOutputCount_; }
#if VITA_AUTO_VERBOSE_LOGGING
    uint32_t outputPictures() const { return outputPictures_; }
#endif

private:
    bool allocateMemBlock(
        const char* name,
        uint32_t size,
        int memoryType,
        uint32_t alignment,
        int& uid,
        void*& base
    );
    void freeMemBlock(int& uid, void*& base);

    bool initialized_ = false;
    bool libraryLoaded_ = false;
    bool libraryInitialized_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t submittedAccessUnits_ = 0;
#if VITA_AUTO_VERBOSE_LOGGING
    uint32_t outputPictures_ = 0;
#endif
    int lastError_ = 0;
    uint32_t lastInitStage_ = 0;
    uint32_t lastOutputCount_ = 0;
    int decoderMemUid_ = -1;
    int esMemUid_ = -1;
    int pictureMemUid_ = -1;
    void* decoderMem_ = nullptr;
    void* esMem_ = nullptr;
    void* pictureMem_ = nullptr;
    bool pictureGpuMapped_ = false;
    SceAvcdecCtrl decoder_{};
    SceAvcdecPicture picture_{};
    SceAvcdecPicture* picturePtr_ = nullptr;
    DecodedFrame latestFrame_{};
};

} // namespace video
