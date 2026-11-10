#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transport/bt_stack.h"
#include "transport/wireless_config.h"
#include "transport/wireless_status.h"

struct VitaAutoBtInfo; // vita_auto_usb.h

class BtBootstrap {
public:
    enum class State {
        Idle,
        WaitingForPhone,
        Handshaking,
        Done,
        Failed
    };

    using Progress = wireless_status::Progress;
    using NameState = wireless_status::NameState;

    ~BtBootstrap() { end(); }

    bool begin(const char* name, const char* recoverOriginal);
    State poll(const WifiNetInfo& net, uint16_t port, const char* password);
    void handOff();

    void end();

    State state() const { return state_; }

private:
    void resetHandshake();
    void restartHandshake();
    void logDiscoverable(const VitaAutoBtInfo& info);
    void trackLinkState(const VitaAutoBtInfo& info, uint64_t nowUs);
    void beginHandshake(const WifiNetInfo& net, uint16_t port, uint64_t nowUs);
    void receiveMessages(const WifiNetInfo& net, const char* password);
    void serviceHandshake(const WifiNetInfo& net, uint16_t port, uint64_t nowUs);
    void waitForPhoneWifi(uint64_t nowUs);
    bool sendMessage(uint16_t type, const std::vector<uint8_t>& payload);
    bool sendStartRequest(const WifiNetInfo& net, uint16_t port);
    bool sendInfoResponse(const WifiNetInfo& net, const char* password);
    void handleMessage(uint16_t type, const uint8_t* payload, size_t size,
                       const WifiNetInfo& net, const char* password);

    struct LogState {
        bool noWifi = false;
        bool noChannel = false;
        int discoverable = -1;
    };

    int lastWriteResult_ = 0;
    LogState log_;
    State state_ = State::Idle;
    BtStack stack_;
    bool infoSent_ = false;
    uint64_t nextResendUs_ = 0;
    uint32_t phoneMessages_ = 0;
    int lastLinkState_ = -1;
    uint64_t linkedSinceUs_ = 0;
    uint64_t handshakeStartUs_ = 0;
    uint64_t infoSentUs_ = 0;
    uint64_t retryAfterUs_ = 0;
    uint64_t doneAtUs_ = 0;
    std::vector<uint8_t> rx_;
};
