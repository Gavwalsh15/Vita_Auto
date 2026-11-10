#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transport/bt_stack.h"
#include "transport/tcp_transport.h"
#include "transport/wireless_status.h"

struct VitaAutoBtInfo; // vita_auto_usb.h

// Bluetooth half of wireless Android Auto. The kernel plugin advertises the
// Android Auto wireless service, so a paired phone connects to the Vita over
// Bluetooth. This class then tells it where to find the Vita on Wi-Fi: the
// phone joins the network and opens a TCP connection to the port the Vita is
// listening on (a TcpTransport in listen mode carries the actual session).
//
// Everything runs on the session owner thread; poll() never blocks.
class BtBootstrap {
public:
    enum class State {
        Idle,            // Not started, or unavailable
        WaitingForPhone, // Advertising; no phone linked yet
        Handshaking,     // Phone linked; exchanging Wi-Fi details
        Done,            // Phone has what it needs; waiting for its TCP connection
        Failed           // Bluetooth unusable on this run (see log)
    };

    // What the waiting screen shows is published through wireless_status.
    using Progress = wireless_status::Progress;
    using NameState = wireless_status::NameState;

    ~BtBootstrap() { end(); }

    // Enables Bluetooth in the kernel plugin. name is what phones see while it
    // runs (empty keeps the Vita's own name); recoverOriginal is the device name
    // a crashed run left behind (empty if none). Returns false when Bluetooth
    // cannot be used (plugin too old, Bluetooth failed); the caller carries on
    // without it.
    bool begin(const char* name, const char* recoverOriginal);

    // One non-blocking step; call about every 100 ms. net is the Wi-Fi network
    // the Vita is on, port is where it listens, password is that network's key.
    State poll(const TcpTransport::WifiNetInfo& net, uint16_t port, const char* password);

    // The phone connected over Wi-Fi: stop handling its messages but keep
    // Bluetooth enabled, because the phone keeps its Bluetooth link to the head
    // unit for the whole wireless session. end() still turns it off later.
    void handOff();

    // Releases the stream and disables Bluetooth. Safe to repeat.
    void end();

    State state() const { return state_; }

private:
    void resetHandshake();
    void restartHandshake();
    void logStackStatus(const VitaAutoBtInfo& info);
    void trackLinkState(const VitaAutoBtInfo& info, uint64_t nowUs);
    void beginHandshake(const TcpTransport::WifiNetInfo& net, uint16_t port, uint64_t nowUs);
    void receiveMessages(const TcpTransport::WifiNetInfo& net, const char* password);
    void serviceHandshake(const TcpTransport::WifiNetInfo& net, uint16_t port, uint64_t nowUs);
    void waitForPhoneWifi(uint64_t nowUs);
    bool sendMessage(uint16_t type, const std::vector<uint8_t>& payload);
    bool sendStartRequest(const TcpTransport::WifiNetInfo& net, uint16_t port);
    bool sendInfoResponse(const TcpTransport::WifiNetInfo& net, const char* password);
    void handleMessage(uint16_t type, const uint8_t* payload, size_t size,
                       const TcpTransport::WifiNetInfo& net, const char* password);

    // What has been logged already, so each line appears once per run (or on a
    // change). Reset as a whole in begin().
    struct LogState {
        bool summary = false;
        bool name = false;
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
    uint64_t doneAtUs_ = 0; // When state_ became Done; 0 while not Done
    std::vector<uint8_t> rx_;
};
