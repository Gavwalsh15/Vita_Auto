// Ported from DiPlay's AudioStream/AudioStreamCodec (GPL-3.0). Feed/emit, no I/O.
#pragma once

#include "airplay.h"
#include "crypto.h"

namespace carplay::core {

constexpr size_t kRtpHeaderBytes = 12;

enum class AudioCodec { Pcm, AacLc, Opus };

struct AudioFormatInfo {
    AudioCodec codec = AudioCodec::Pcm;
    uint32_t sample_rate = 44100;
    uint32_t channels = 2;
};

AudioFormatInfo audio_format_info(uint64_t format_bits);

class AudioPacketReceiver {
public:
    explicit AudioPacketReceiver(MediaHost &sink) : sink_(sink) {}
    void reset(int stream_type, const uint8_t key[32], uint64_t format_bits);
    void feed(const uint8_t *datagram, size_t length);
    bool active() const { return active_; }
    int stream_type() const { return type_; }
    AudioFormatInfo format() const { return format_; }
    uint32_t packets() const { return packets_; }
    uint32_t rejected() const { return rejected_; }

private:
    MediaHost &sink_;
    uint8_t key_[32] = {};
    int type_ = 0;
    AudioFormatInfo format_;
    bool active_ = false;
    uint32_t packets_ = 0, rejected_ = 0;
};

} // namespace carplay::core
