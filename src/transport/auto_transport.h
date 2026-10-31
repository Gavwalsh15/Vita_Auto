#pragma once

#include "transport/tcp_transport.h"
#include "transport/usb_transport.h"


class AutoTransport : public Transport {
public:
    struct Options {
        const char* wifiHostOverride = "";
        bool usb = true;
        bool headunitServer = true;
    };

    explicit AutoTransport(const Options& options);
    ~AutoTransport() override { close(); }

    bool open(bool (*cancelled)()) override;
    int close() override;
    bool send(const uint8_t* data, size_t size) override;
    int receive(
        std::vector<uint8_t>& data,
        UsbRxTiming* timing = nullptr
    ) override;

    bool connected() const override { return active_ != nullptr && active_->connected(); }
    bool versionNegotiated() const override
    {
        return active_ != nullptr && active_->versionNegotiated();
    }
    bool wireless() const override
    {
        return active_ != nullptr && active_->wireless();
    }

    int beginShutdownDrain() override
    {
        return active_ != nullptr ? active_->beginShutdownDrain() : 0;
    }
    int shutdownDrainState(TransportDrainState& state) override
    {
        return active_ != nullptr ? active_->shutdownDrainState(state) : 0;
    }
    uint32_t mediaMaxUnacked() const override
    {
        return active_ != nullptr ? active_->mediaMaxUnacked() : 1;
    }
    void logDiagnostics(const char* reason) const override
    {
        if (active_ != nullptr)
            active_->logDiagnostics(reason);
    }

    app::AppError lastAppError() const override;
    int lastError() const override;

private:
    // Closes everything except the winner. False if the USB stop failed.
    bool closeLosers(Transport* winner);

    bool useUsb_;
    bool useHeadunitServer_;
    UsbTransport usb_;
    TcpTransport tcp_;       // dials the phone's Headunit Server
    Transport* active_ = nullptr;
};
