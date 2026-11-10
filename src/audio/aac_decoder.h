#pragma once

#include <psp2/audiodec.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

// AAC-LC through the Vita's hardware decoder (SceAudiodec). The phone sends raw
// access units; the decoder wants ADTS, so each unit gets a header in front.
class AacDecoder {
public:
    AacDecoder() = default;
    ~AacDecoder();

    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    bool open(uint32_t sampleRate, uint8_t channels);
    void close();
    bool isOpen() const { return open_; }
    int lastError() const { return lastError_; }

    // One access unit in, interleaved 16-bit samples out (count of int16_t
    // values). Returns -1 on a decoder error.
    int decode(const uint8_t* unit, size_t length, std::vector<int16_t>& out);

private:
    bool create(uint32_t infoSize);

    bool open_ = false;
    bool libraryUp_ = false;
    bool moduleLoaded_ = false;
    uint32_t sampleRate_ = 0;
    uint8_t channels_ = 0;
    int lastError_ = 0;
    SceAudiodecCtrl ctrl_{};
    SceAudiodecInfo info_{};
    std::vector<uint8_t> esStorage_;
    std::vector<uint8_t> pcmStorage_;
    uint8_t* es_ = nullptr;
    uint8_t* pcm_ = nullptr;
};

} // namespace audio
