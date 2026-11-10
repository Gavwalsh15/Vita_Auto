#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transport/wireless_config.h"

namespace carplay::core { class Authenticator; }

namespace carplay {

struct Handoff {
    char name[32] = "Vita Auto";
    char ssid[33] = {};
    char passphrase[64] = {};
    uint8_t security = 2; // 0 none, 1 WEP, 2 WPA/WPA2, 3 WPA3 transition, 4 WPA3
    uint8_t channel = 6;
    char ip[16] = {};
    uint16_t airplayPort = 7000;
    char deviceId[18] = {}; // "02:AB:CD:EF:01:23"
    uint8_t mac[6] = {};    // the same, as bytes
    uint8_t btAddress[6] = {};
    bool btAddressKnown = false;
    char publicKeyHex[65] = {}; // the AirPlay identity's Ed25519 public key
    char sourceVersion[16] = "366.0";
};

enum class Iap2Role { Bluetooth, Tunnel };

class Iap2Endpoint {
public:
    Iap2Endpoint(const Handoff& handoff, core::Authenticator& authenticator, Iap2Role role);
    ~Iap2Endpoint();
    Iap2Endpoint(const Iap2Endpoint&) = delete;
    Iap2Endpoint& operator=(const Iap2Endpoint&) = delete;

    bool start(int64_t nowMs);
    void feed(const uint8_t* data, size_t size, int64_t nowMs);
    void tick(int64_t nowMs);
    void takeOutput(std::vector<uint8_t>& out);

    bool authSucceeded() const;
    bool handoffSent() const;
    uint32_t messages() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace carplay
