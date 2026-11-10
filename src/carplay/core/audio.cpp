#include "audio.h"

namespace carplay::core {

namespace {

const size_t kNonceBytes = 8;

uint32_t be32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           static_cast<uint32_t>(p[3]);
}

void put_be32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

} // namespace

AudioFormatInfo audio_format_info(uint64_t bits) {
    AudioFormatInfo info;
    if (bits & 0x70000000ull) {
        info.codec = AudioCodec::Opus;
        info.sample_rate = 48000;
        info.channels = 1;
        return info;
    }
    if (bits & (0x400000ull | 0x800000ull)) {
        info.codec = AudioCodec::AacLc;
        info.sample_rate = bits & 0x800000ull ? 48000 : 44100;
        return info;
    }

    static const struct {
        uint32_t bit, rate, channels;
    } kPcm[] = {{0x4, 8000, 1},     {0x8, 8000, 2},     {0x10, 16000, 1},   {0x20, 16000, 2},
                {0x40, 24000, 1},   {0x80, 24000, 2},   {0x100, 32000, 1},  {0x200, 32000, 2},
                {0x400, 44100, 1},  {0x800, 44100, 2},  {0x4000, 48000, 1}, {0x8000, 48000, 2}};
    for (const auto &entry : kPcm)
        if (bits == entry.bit) {
            info.sample_rate = entry.rate;
            info.channels = entry.channels;
        }
    return info;
}

void AudioPacketReceiver::reset(int stream_type, const uint8_t key[32], uint64_t format_bits) {
    memcpy(key_, key, sizeof key_);
    type_ = stream_type;
    format_ = audio_format_info(format_bits);
    active_ = true;
    packets_ = rejected_ = 0;
}

void AudioPacketReceiver::feed(const uint8_t *datagram, size_t length) {
    if (!active_ || length < kRtpHeaderBytes + kAeadTagBytes + kNonceBytes) {
        ++rejected_;
        return;
    }
    uint8_t nonce[12] = {};
    memcpy(nonce + 4, datagram + length - kNonceBytes, kNonceBytes);
    Bytes payload;
    if (!aead_open(key_, nonce, datagram + 4, kRtpHeaderBytes - 4, datagram + kRtpHeaderBytes,
                   length - kRtpHeaderBytes - kNonceBytes, payload)) {
        ++rejected_;
        return;
    }
    ++packets_;
    sink_.on_audio(type_, be32(datagram + 4), payload.data(), payload.size());
}

} // namespace carplay::core
