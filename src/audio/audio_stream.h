#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/aac_decoder.h"
#include "audio/audio_output.h"

namespace audio {

enum class Encoding { Pcm16Le, Pcm16Be, AacLc };

class Stream {
public:
    explicit Stream(const char* name) : name_(name) {}

    bool configure(Encoding encoding, uint32_t sampleRate, uint8_t channels, uint32_t bufferMs);
    void reset();
    bool configured() const { return output_ != nullptr; }

    bool push(const uint8_t* data, size_t size);

    uint32_t packets() const { return packets_; }
    uint32_t dropped() const { return dropped_; }
    void logStats(const char* prefix) const;

private:
    bool drop(const char* why, int error);

    const char* name_;
    Encoding encoding_ = Encoding::Pcm16Le;
    uint32_t rate_ = 0;
    uint8_t channels_ = 0;
    uint32_t bufferMs_ = 0;
    std::unique_ptr<Output> output_;
    std::unique_ptr<AacDecoder> aac_;
    std::vector<int16_t> samples_;
    uint32_t packets_ = 0;
    uint64_t bytes_ = 0;
    uint32_t dropped_ = 0;
};

} // namespace audio
