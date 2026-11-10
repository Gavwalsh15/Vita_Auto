#pragma once

#include <cstdint>
#include <cstdio>

#include "transport/transport.h"

// The SDK network stack, shared by everything that opens sockets. acquire()
// returns 0 or a negative SDK error; every successful acquire needs a release().
// Call from the session owner thread only.
namespace net_stack {
int acquire();
void release();
} // namespace net_stack

// Wi-Fi transport for wireless Android Auto. The Vita joins the phone's hotspot
// and connects to the Headunit Server the phone exposes in Android Auto's
// developer settings. Unlike USB, nothing negotiates the AA version for us, so
// the session sends VERSION_REQUEST itself.
//
// With a nonzero listenPort the roles flip: the Vita listens and waits for the
// phone to connect (the standard wireless Android Auto flow that the Bluetooth
// bootstrap sets up), instead of dialling the phone.
class TcpTransport : public Transport {
public:
    // What the Bluetooth bootstrap tells the phone about the network the Vita
    // is on. Strings are empty when unknown.
    struct WifiNetInfo {
        char ip[16] = {};
        char ssid[33] = {};
        char bssid[18] = {};

        void setBssid(const uint8_t* mac)
        {
            std::snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    };

    // hostOverride is the phone's IP when it is not the hotspot gateway.
    explicit TcpTransport(const char* hostOverride = "", uint16_t listenPort = 0);
    ~TcpTransport() override { close(); }

    // Waits for the phone until cancelled; fails at once if the Vita is not on Wi-Fi.
    bool open(bool (*cancelled)()) override;
    // open() split into a start and a non-blocking step (call about every
    // 100 ms). With strict false, not being on Wi-Fi yet is not an error: the
    // transport keeps checking, so it can run beside USB.
    bool beginOpen(bool (*cancelled)(), bool strict);
    OpenPoll pollOpen(bool (*cancelled)());

    int close() override;
    bool send(const uint8_t* data, size_t size) override;
    int receive(
        std::vector<uint8_t>& data,
        UsbRxTiming* timing = nullptr
    ) override;

    bool connected() const override { return connected_; }
    bool versionNegotiated() const override { return false; }

    void logDiagnostics(const char* reason) const override;
    // Wi-Fi round trips are 5-50 ms.
    uint32_t mediaMaxUnacked() const override { return 3; }

    // True once the Vita is on a network; fills in what the phone must join.
    // Valid after beginOpen succeeded.
    bool queryNetInfo(WifiNetInfo& info) const;

    // Listen mode only: the network is the Vita's own access point, which the
    // system's connection manager knows nothing about. With info set, the
    // transport treats Wi-Fi as ready and queryNetInfo returns it; nullptr
    // goes back to asking the system.
    void setLocalNetwork(const WifiNetInfo* info);

private:
    bool serverMode() const { return listenPort_ != 0; }
    bool initNetwork();
    OpenPoll pollListen(uint64_t nowUs);
    void closeListen();
    // AppError::None on success; otherwise why no host could be found.
    app::AppError lookupHost(int* detail);
    OpenPoll startAttempt(uint64_t nowUs);
    OpenPoll finishAttempt();
    void abandonAttempt(uint64_t nowUs, const char* stage, int detail);
    void closeAttempt();
    void closeSocket();

    char hostOverride_[40] = {};
    char host_[64] = {};
    uint16_t listenPort_ = 0;
    bool localNetwork_ = false;
    WifiNetInfo localNet_;
    int listenSocket_ = -1;
    bool holdsNet_ = false;

    bool wifiReady_ = false;
    int attemptSocket_ = -1;
    int attemptEpoll_ = -1;
    uint64_t attemptDeadlineUs_ = 0;
    uint64_t nextAttemptUs_ = 0;
    uint64_t nextWifiCheckUs_ = 0;
    unsigned attempts_ = 0;

    int socket_ = -1;
    int rxEpoll_ = -1;
    bool connected_ = false;
    uint32_t sentBytes_ = 0;
    uint32_t receivedBytes_ = 0;

    // Stutter diagnostics: gaps between data arriving (network) and between
    // receive() calls (the session loop on the Vita).
    uint64_t lastDataUs_ = 0;
    uint64_t lastCallUs_ = 0;
    uint64_t nextGapLogUs_ = 0;
    uint64_t maxRxGapUs_ = 0;
    uint64_t maxLoopGapUs_ = 0;
    uint32_t rxGaps_ = 0;
    uint32_t loopGaps_ = 0;
};
