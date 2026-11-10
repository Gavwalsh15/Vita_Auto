#pragma once

// Session's internals, shared by the files that implement it
// (carplay_session.cpp, carplay_media.cpp, carplay_input.cpp).

#include "carplay/carplay_session.h"

#include "audio/audio_stream.h"
#include "carplay/airplay_control.h"
#include "carplay/event_link.h"
#include "inputs/input_events.h"

#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>

#include <memory>
#include <vector>

#include "carplay/core/airplay.h"
#include "carplay/core/audio.h"
#include "carplay/core/crypto.h"
#include "carplay/core/events.h"
#include "carplay/core/media.h"
#include "carplay/core/pairing.h"
#include "carplay/core/rtsp.h"
#include "carplay/core/screen.h"
#include "carplay/core/timing.h"

namespace carplay {

using core::Bytes;

constexpr size_t RX_CHUNK = 16 * 1024;

enum Slot {
    ControlListen, ControlConn, EventListen, Event, ScreenListen, Screen, TunnelListen, Tunnel,
    UdpTiming, UdpAudioMain, UdpAudioAlt, UdpAudioHigh, UdpAudioControl, UdpKeepAlive,
    SlotCount
};

class VitaRandom final : public core::Random {
public:
    bool fill(uint8_t* out, size_t length) override
    {
        return sceKernelGetRandomNumber(out, length) >= 0;
    }
};

inline uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

struct Session::Impl final : core::MediaListeners,
                             core::ScreenSink,
                             core::AudioSink,
                             TunnelHost {
    Impl(const SessionConfig& c, core::Authenticator& a) : config(c), auth(a)
    {
        for (int& fd : fds)
            fd = -1;
    }

    struct Conn {
        Conn(Impl& owner)
            : security(owner.identity, owner.store, owner.random),
              media(security, owner, owner, owner),
              control(owner.info, security, media, owner)
        {
            security.set_authenticator(&owner.auth);
        }
        core::PairingSecurity security;
        core::SessionMedia media;
        Control control;
        EventLink events;
        std::unique_ptr<Iap2Endpoint> tunnel;
        core::ControlCipher tunnelCipher;
        Bytes tunnelEncrypted, tunnelPlain;
        SceNetInAddr peer{};
        bool saved = false;
        uint64_t nextTimingUs = 0;
    };

    bool begin();
    void end();
    void service(uint32_t waitUs);
    void closeConnection(const char* why);

    void watch(Slot slot, int fd);
    void closeSlot(Slot slot);
    void queue(Slot slot, const Bytes& data);
    void queue(Slot slot, const uint8_t* data, size_t size);
    void flush();
    void handleSlot(Slot slot);
    void acceptOn(Slot listen, Slot connection, const char* what);
    void readStream(Slot slot);
    void readDatagrams(Slot slot);
    void onControlData(const uint8_t* data, size_t size);
    void onEventData(const uint8_t* data, size_t size);
    void onScreenData(const uint8_t* data, size_t size);
    void onTunnelData(const uint8_t* data, size_t size);
    void pumpTunnel();
    void sendTiming(uint64_t now);
    int64_t timingNowNs() const { return static_cast<int64_t>(nowUs()) * 1000 + timingBaseNs; }
    void saveState();
    void summary(uint64_t now);

    bool sendInput(const inputs::InputEvent& event);
    bool sendTouch(const inputs::TouchEvent& touch);
    bool sendButton(const inputs::ButtonEvent& button);
    bool sendKnob(const core::KnobState& state);
    bool sendHid(const char* uid, const uint8_t* report, size_t size);
    void drainInput();

    bool open(core::MediaChannel channel, uint32_t& port) override;
    void close(core::MediaChannel channel) override;
    void on_config(core::VideoCodec codec, const uint8_t* record, size_t length) override;
    void on_frame(const uint8_t* annexB, size_t length, uint64_t senderNanos) override;
    void on_audio(int streamType, uint32_t rtpTimestamp, const uint8_t* payload, size_t length) override;
    bool openTunnel(uint64_t seed, uint32_t& port) override;
    void closeTunnel() override;
    bool openKeepAlive(uint32_t& port) override;

    SessionConfig config;
    core::Authenticator& auth;
    VitaRandom random;
    core::AirPlayIdentity identity;
    core::PairingStore store;
    InfoConfig info;
    char publicKeyHex[65] = {};
    char deviceId[18] = {};
    char bluetoothAddress[18] = {};
    std::unique_ptr<Conn> conn;

    int epoll = -1;
    int fds[SlotCount];
    std::vector<uint8_t> tx[SlotCount];
    std::vector<uint8_t> rx = std::vector<uint8_t>(RX_CHUNK);
    bool begun = false;

    std::vector<uint8_t> parameterSets;
    bool sendParameterSets = false;
    std::vector<uint8_t> frameBuffer;
    uint32_t videoSequence = 0;
    uint32_t videoFrames = 0;
    uint32_t videoConfigs = 0;
    bool videoLogged = false;

    audio::Stream audioStreams[3] = {audio::Stream("main"), audio::Stream("alt"), audio::Stream("main-high")};
    uint32_t audioUnsupported = 0;

    core::KnobState knob{};
    uint32_t touchX[TOUCH_CONTACTS] = {};
    uint32_t touchY[TOUCH_CONTACTS] = {};
    uint32_t hidSent = 0;
    uint64_t nextSummaryUs = 0;

    int64_t timingBaseNs = 0;
};

} // namespace carplay
