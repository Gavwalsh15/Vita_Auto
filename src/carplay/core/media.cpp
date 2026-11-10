#include "media.h"

namespace carplay::core {

MediaChannel audio_channel(int stream_type) {
    return stream_type == kStreamAltAudio       ? MediaChannel::AudioAlt
           : stream_type == kStreamMainHighAudio ? MediaChannel::AudioMainHigh
                                                 : MediaChannel::AudioMain;
}

namespace {

int audio_index(MediaChannel channel) {
    return channel == MediaChannel::AudioAlt ? 1 : channel == MediaChannel::AudioMainHigh ? 2 : 0;
}

} // namespace

bool SessionMedia::open_session(uint32_t peer_timing_port, uint32_t &timing_port, uint32_t &event_port) {
    if (!host_.open(MediaChannel::Event, event_port) || !host_.open(MediaChannel::Timing, timing_port))
        return false;
    peer_timing_port_ = peer_timing_port;
    session_open_ = true;
    return true;
}

void SessionMedia::stream_key(const Plist &stream, uint64_t &connection_id, uint8_t key[32]) const {
    // The phone names the stream; the key is bound to that number.
    const Plist *id = stream.get("streamConnectionID");
    int64_t value = 0;
    connection_id = id && id->to_int(value) ? static_cast<uint64_t>(value) : 0;
    screen_stream_key(security_.shared_secret(), connection_id, key);
}

bool SessionMedia::open_screen(int, const Plist &stream, uint32_t &data_port) {
    uint8_t key[32];
    stream_key(stream, connection_id_, key);
    if (!host_.open(MediaChannel::Screen, data_port)) return false;
    screen_.reset(key);
    memset(key, 0, sizeof key);
    screen_open_ = true;
    return true;
}

bool SessionMedia::open_audio(int type, const Plist &stream, uint32_t &data_port, uint32_t &control_port) {
    const MediaChannel channel = audio_channel(type);
    if (!host_.open(channel, data_port)) return false;
    if (!audio_control_port_ && !host_.open(MediaChannel::AudioControl, audio_control_port_)) return false;
    control_port = audio_control_port_;
    uint8_t key[32];
    uint64_t connection_id = 0;
    stream_key(stream, connection_id, key);
    const Plist *format = stream.get("audioFormat");
    int64_t bits = 0;
    if (format) format->to_int(bits);
    audio_[audio_index(channel)].reset(type, key, static_cast<uint64_t>(bits));
    memset(key, 0, sizeof key);
    return true;
}

void SessionMedia::close_stream(int type) {
    if (type == kStreamMainScreen || type == kStreamAltScreen) {
        host_.close(MediaChannel::Screen);
        screen_open_ = false;
    } else if (type >= kStreamMainAudio && type <= kStreamMainHighAudio) {
        host_.close(audio_channel(type));
    }
}

void SessionMedia::screen_connected() {
    uint8_t key[32];
    screen_stream_key(security_.shared_secret(), connection_id_, key);
    screen_.reset(key);
    memset(key, 0, sizeof key);
}

bool SessionMedia::feed_screen(const uint8_t *data, size_t length) {
    return screen_open_ && screen_.feed(data, length);
}

void SessionMedia::feed_audio(MediaChannel channel, const uint8_t *data, size_t length) {
    audio_[audio_index(channel)].feed(data, length);
}

const AudioPacketReceiver &SessionMedia::audio(MediaChannel channel) const { return audio_[audio_index(channel)]; }

uint32_t SessionMedia::audio_packets() const { return audio_[0].packets() + audio_[1].packets() + audio_[2].packets(); }

} // namespace carplay::core
