#pragma once

#include <stddef.h>
#include <stdint.h>

namespace carplay::core {

constexpr int kStreamMainAudio = 100;
constexpr int kStreamAltAudio = 101;
constexpr int kStreamMainHighAudio = 102;
constexpr int kStreamMainScreen = 110;
constexpr int kStreamAltScreen = 111;
constexpr int kStreamData = 130;

enum class MediaChannel { Event, Screen, Timing, AudioMain, AudioAlt, AudioMainHigh, AudioControl };
constexpr int kMediaChannels = 7;

enum class VideoCodec { H264, H265 };

class MediaHost {
public:
    virtual ~MediaHost() = default;
    virtual bool open(MediaChannel channel, uint32_t &port) = 0;
    virtual void close(MediaChannel channel) = 0;
    virtual void on_config(VideoCodec codec, const uint8_t *record, size_t length) = 0;
    virtual void on_frame(const uint8_t *annex_b, size_t length, uint64_t sender_nanos) = 0;
    virtual void on_audio(int stream_type, uint32_t rtp_timestamp, const uint8_t *payload, size_t length) = 0;
    virtual bool open_tunnel(uint64_t seed, uint32_t &port) = 0;
    virtual void close_tunnel() = 0;
    // The phone's low-power keep-alive datagrams arrive here; nothing is read from them.
    virtual bool open_keep_alive(uint32_t &port) = 0;
};

} // namespace carplay::core
