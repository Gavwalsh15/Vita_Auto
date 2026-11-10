#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace nbtplay { class Authenticator; }

namespace carplay {

// "02:AB:CD:EF:01:23" into out, which must hold 18 bytes.
inline void formatMac(char* out, const uint8_t mac[6])
{
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
}

// What the phone is told so it can reach the AirPlay receiver: the Wi-Fi network
// and where on it the receiver listens.
struct Handoff {
    char name[32] = "Vita Auto";
    char ssid[33] = {};
    char passphrase[64] = {};
    uint8_t security = 2; // 0 none, 1 WEP, 2 WPA/WPA2, 3 WPA3 transition, 4 WPA3
    uint8_t channel = 6;
    char ip[16] = {};
    uint16_t airplayPort = 7000;
    char deviceId[18] = {}; // "02:AB:CD:EF:01:23", the AirPlay device id (the Wi-Fi address)
    uint8_t mac[6] = {};    // the same, as bytes
    // The Vita's real Bluetooth address, which the identification and /info must
    // name: the phone ties the CarPlay audio route to the Bluetooth device it is
    // linked with. Unknown until the Bluetooth stack reports it; then the Wi-Fi
    // address stands in (and audio never opens).
    uint8_t btAddress[6] = {};
    bool btAddressKnown = false;
    char publicKeyHex[65] = {}; // the AirPlay identity's Ed25519 public key
    char sourceVersion[16] = "366.0";
};

// Where the iAP2 bytes travel. Bluetooth: the RFCOMM pipe, wireless link profile,
// the phone starts the link. Tunnel: the AirPlay data stream after the Wi-Fi
// handoff, wired link profile, the accessory starts the link; the phone runs the
// whole exchange again there and then expects update subscriptions.
enum class Iap2Role { Bluetooth, Tunnel };

// One iAP2 connection from the accessory side: the link layer, identification,
// authentication, and the wireless CarPlay handoff messages. Does no I/O: bytes
// from the phone go in through feed(), bytes for it come out of takeOutput().
class Iap2Endpoint {
public:
    Iap2Endpoint(const Handoff& handoff, nbtplay::Authenticator& authenticator, Iap2Role role);
    ~Iap2Endpoint();
    Iap2Endpoint(const Iap2Endpoint&) = delete;
    Iap2Endpoint& operator=(const Iap2Endpoint&) = delete;

    bool start(int64_t nowMs);
    void feed(const uint8_t* data, size_t size, int64_t nowMs);
    // Runs the link's timers (retransmissions, acknowledgements).
    void tick(int64_t nowMs);
    // Appends what the link wants sent to out and clears it.
    void takeOutput(std::vector<uint8_t>& out);

    bool authSucceeded() const;
    bool handoffSent() const;
    uint32_t messages() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace carplay
