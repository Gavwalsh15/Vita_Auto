#pragma once

#include <cstdint>

#include "transport/wifi_ap.h"
#include "transport/wireless_config.h"

class PhoneNetwork {
public:
    struct Info : WifiNetInfo {
        char password[64] = {};
        int channel = 0;
        bool ownAccessPoint = false;
    };

    ~PhoneNetwork() { end(); }

    void begin(const WirelessConfig& config, const char* tag);
    void poll();
    bool starting() const { return apStarted_ && !apUp_; }
    bool accessPointFailed() const { return apFailed_; }
    bool current(Info& info) const;
    bool accessPointUp() const { return apUp_; }
    void end();

private:
    WirelessConfig config_;
    const char* tag_ = "";
    WifiAp ap_;
    bool apStarted_ = false;
    bool apUp_ = false;
    bool apFailed_ = false;
    uint64_t nextPollUs_ = 0;
};
