#pragma once

#include "airplay.h"
#include "audio.h"
#include "bplist.h"
#include "events.h"
#include "pairing.h"
#include "screen.h"
#include "timing.h"

namespace carplay::core {

MediaChannel audio_channel(int stream_type);

class SessionMedia {
public:
    SessionMedia(const PairingSecurity &security, MediaHost &host)
        : security_(security), host_(host), screen_(host),
          audio_{AudioPacketReceiver(host), AudioPacketReceiver(host), AudioPacketReceiver(host)} {}

    bool open_session(uint32_t peer_timing_port, uint32_t &timing_port, uint32_t &event_port);
    // Screen stream (110 main, 111 alternate): H.264 over TCP to data_port.
    bool open_screen(int type, const Plist &stream, uint32_t &data_port);
    // Audio stream (100, 101, 102): RTP over UDP to data_port, control on control_port.
    bool open_audio(int type, const Plist &stream, uint32_t &data_port, uint32_t &control_port);
    void session_active() { recording_ = true; } // RECORD
    void close_stream(int type);

    void screen_connected();
    bool feed_screen(const uint8_t *data, size_t length);

    uint32_t peer_timing_port() const { return peer_timing_port_; }
    void timing_request(int64_t now_ns, uint8_t packet[kTimingPacketBytes]) { clock_.request(now_ns, packet); }
    bool feed_timing(const uint8_t *data, size_t length, int64_t now_ns, uint8_t reply[kTimingPacketBytes]) {
        return clock_.on_packet(data, length, now_ns, reply);
    }
    const TimingClock &clock() const { return clock_; }

    void feed_audio(MediaChannel channel, const uint8_t *data, size_t length);
    const AudioPacketReceiver &audio(MediaChannel channel) const;
    uint32_t audio_packets() const;

    bool recording() const { return recording_; }
    uint32_t frames() const { return screen_.frames(); }
    uint32_t configs() const { return screen_.configs(); }

private:
    void stream_key(const Plist &stream, uint64_t &connection_id, uint8_t key[32]) const;

    const PairingSecurity &security_;
    MediaHost &host_;
    ScreenStreamReceiver screen_;
    TimingClock clock_;
    AudioPacketReceiver audio_[3];
    uint64_t connection_id_ = 0;
    uint32_t peer_timing_port_ = 0, audio_control_port_ = 0;
    bool screen_open_ = false, recording_ = false, session_open_ = false;
};

} // namespace carplay::core
