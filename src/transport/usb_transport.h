#pragma once

#include "transport/transport.h"

// User-mode half of the Android USB transport. The kernel plugin
// owns USB enumeration and bulk endpoints; this class only exchanges bytes
// once that plugin has completed the AA version handshake.
class UsbTransport : public Transport {
public:
    // False when vita_auto_usb.skprx is not installed: the client bridge
    // imports its exports, so it cannot load without it. Cached after the first call.
    static bool pluginAvailable();

    bool open(bool (*cancelled)()) override;
    // open() split into a one-time start and a non-blocking step, so another
    // transport can be polled between steps. Call pollOpen about every 100 ms.
    bool beginOpen(bool (*cancelled)());
    OpenPoll pollOpen(bool (*cancelled)());
    // Keeps the bridge loaded; failed cleanup can be retried with another close().
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

    // Wake handler for Transport::setWakeHandler.
    static void wakeKernel();

private:
    bool connected_ = false;
    bool sessionStarted_ = false;
    uint64_t connectionDeadlineUs_ = 0;
    int lastState_ = -1;
    unsigned int lastVidPid_ = 0xffffffffu;
};
