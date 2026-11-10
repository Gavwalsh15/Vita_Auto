#pragma once

#include "transport/bt_bootstrap.h"
#include "transport/tcp_transport.h"
#include "transport/wireless_config.h"
#include "transport/wifi_ap.h"

class WirelessBootstrap {
public:
    enum class Result { Waiting, Connected, Stopped, Failed };

    explicit WirelessBootstrap(const WirelessConfig& config)
        : config_(config), listener_("", LISTEN_PORT) {}
    ~WirelessBootstrap() { end(); }

    bool enabled() const { return config_.bluetooth; }

    bool begin(bool (*cancelled)());
    Result poll(bool (*cancelled)());

    Transport& transport() { return listener_; }
    void handOff();
    void end();

    app::AppError lastAppError() const { return listener_.lastAppError(); }
    int lastError() const { return listener_.lastError(); }

private:
    static constexpr uint16_t LISTEN_PORT = 5288;

    void endAccessPoint();

    WirelessConfig config_;
    TcpTransport listener_;
    BtBootstrap bt_;
    WifiAp ap_;
    bool apWaiting_ = false;
    bool apUp_ = false;
};
