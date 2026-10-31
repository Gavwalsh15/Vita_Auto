#pragma once

#include "transport/transport.h"

// User-mode half of the Android USB transport. The kernel plugin
// owns USB enumeration and bulk endpoints; this class only exchanges bytes
// once that plugin has completed the AA version handshake.
class UsbTransport : public Transport {
public:

    static bool pluginAvailable();

    bool open(bool (*cancelled)()) override;
    bool beginOpen(bool (*cancelled)());
    OpenPoll pollOpen(bool (*cancelled)());
    int close() override;
    bool send(const uint8_t* data, size_t size) override;
    int receive(
        std::vector<uint8_t>& data,
        UsbRxTiming* timing = nullptr
    ) override;

    bool connected() const override;
    bool versionNegotiated() const override { return true; }

    int beginShutdownDrain() override;
    int shutdownDrainState(TransportDrainState& state) override;
    void logDiagnostics(const char* reason) const override;

    // Rate-limited liveness line: proves the IO loop is running even while no
    // frames arrive, and shows how long the stream has been quiet.
    void logHeartbeat(uint32_t videoFrames) override;

    static void wakeKernel();

private:
    bool connected_ = false;
    bool sessionStarted_ = false;
    uint64_t connectionDeadlineUs_ = 0;
    int lastState_ = -1;
    unsigned int lastVidPid_ = 0xffffffffu;
    uint64_t lastRxAtUs_ = 0;
    uint64_t nextHeartbeatUs_ = 0;
};
