#pragma once

#include "transport/transport.h"


class TcpTransport : public Transport {
public:
    explicit TcpTransport(const char* hostOverride = "");
    ~TcpTransport() override { close(); }

    bool open(bool (*cancelled)()) override;
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
    bool wireless() const override { return true; }

private:
    bool initNetwork();
    app::AppError lookupHost(int* detail);
    OpenPoll startAttempt(uint64_t nowUs);
    OpenPoll finishAttempt();
    void abandonAttempt(uint64_t nowUs, const char* stage, int detail);
    void closeAttempt();
    void closeSocket();

    char hostOverride_[40] = {};
    char host_[64] = {};
    bool holdsNet_ = false;

    bool wifiReady_ = false;
    int attemptSocket_ = -1;
    int attemptEpoll_ = -1;
    uint64_t attemptDeadlineUs_ = 0;
    uint64_t nextAttemptUs_ = 0;
    uint64_t nextWifiCheckUs_ = 0;
    unsigned attempts_ = 0;
    const char* connectStage_ = "none";
    int connectDetail_ = 0;

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
