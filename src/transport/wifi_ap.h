#pragma once

#include <psp2/types.h>

#include <cstdint>

#include "transport/wireless_config.h"

struct VitaAutoApInfo; // vita_auto_usb.h

class WifiAp {
public:
    enum class State { Off, Starting, Up, Failed };

    struct Config {
        const char* ssid = "";
        const char* password = ""; // Empty: open network; else 8-63 characters
        int channel = 6;
    };

    ~WifiAp() { end(); }

    bool begin(const Config& config);
    State poll();
    void end();

    State state() const { return state_; }
    const WifiNetInfo& netInfo() const { return net_; }

private:
    static int keepAliveMain(SceSize args, void* argp);
    void logInfo(const VitaAutoApInfo& info, bool force);
    void dhcpOpen();
    void dhcpClose();
    void dhcpPoll();
    uint32_t dhcpLease(const uint8_t* chaddr);

    State state_ = State::Off;
    SceUID keepAliveThread_ = -1;
    volatile bool keepAliveRun_ = false;
    WifiNetInfo net_;
    uint64_t nextLogUs_ = 0;
    uint32_t loggedAssociations_ = 0;
    uint32_t loggedDhcp_ = 0;
    uint32_t loggedDhcpOut_ = 0;

    volatile int dhcpSocket_ = -1;
    int dhcpSocketResult_ = 0;
    volatile uint32_t udpIn_ = 0;      // Anything received on port 67
    volatile uint32_t dhcpIn_ = 0;     // DISCOVER/REQUEST
    volatile uint32_t dhcpOut_ = 0;    // OFFER/ACK/NAK sent
    volatile int dhcpLastSend_ = 0;
    volatile uint32_t dhcpLastOffer_ = 0;
    uint8_t dhcpLeases_[4][6] = {};
    uint32_t dhcpLeaseCount_ = 0;
    uint32_t dhcpNextVictim_ = 0; // Slot to reuse once all are taken
    uint8_t dhcpPacket_[576] = {};
};
