#pragma once

#include "bplist.h"
#include "rt.h"
#include "rtsp.h"

namespace carplay::core {

constexpr int kStreamMainAudio = 100;
constexpr int kStreamAltAudio = 101;
constexpr int kStreamMainHighAudio = 102;
constexpr int kStreamMainScreen = 110;
constexpr int kStreamAltScreen = 111;
constexpr int kStreamData = 130;

class AirPlaySecurity {
public:
    virtual ~AirPlaySecurity() = default;
    virtual const char *name() const = 0;
    virtual int pair_setup(const Bytes &body, Bytes &reply) = 0;
    virtual int pair_verify(const Bytes &body, Bytes &reply) = 0;
    virtual int auth_setup(const Bytes &body, Bytes &reply) = 0;
    virtual bool channel_encrypted() const = 0;
    virtual bool decrypt(Bytes &in, Bytes &plain) = 0;
    virtual bool encrypt(const uint8_t *data, size_t length, Bytes &out) = 0;
};

class AirPlayMedia {
public:
    virtual ~AirPlayMedia() = default;
    virtual bool open_session(uint32_t peer_timing_port, uint32_t &timing_port, uint32_t &event_port) = 0;
    // Screen stream (110 main, 111 alternate): H.264 over TCP to data_port.
    virtual bool open_screen(int type, const Plist &stream, uint32_t &data_port) = 0;
    // Audio stream (100, 101, 102): RTP over UDP to data_port, control on control_port.
    virtual bool open_audio(int type, const Plist &stream, uint32_t &data_port, uint32_t &control_port) = 0;
    virtual void session_active() = 0; // RECORD
    virtual void close_stream(int type) = 0;
};

} // namespace carplay::core
