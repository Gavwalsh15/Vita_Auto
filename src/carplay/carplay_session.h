#pragma once

#include <cstdint>

#include "carplay/audio_offer.h"
#include "carplay/iap2_endpoint.h"

namespace carplay::core { class Authenticator; }

namespace carplay {

struct SessionConfig {
    Handoff handoff;            // what the phone is told; filled in by the owner
    char statePath[160] = {};   // where the pairing identity is kept (empty: not kept)
    AudioOffer audioOffer = AudioOffer::Full;
};

// The Wi-Fi side of CarPlay: the AirPlay receiver the phone connects to after Bluetooth points it at the
// Vita. Owns the sockets, runs pairing and the state machines, and routes screen, audio, touch
// and buttons. Everything runs on the thread that calls service().
class Session {
public:
    Session(const SessionConfig& config, core::Authenticator& authenticator);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Loads or creates the pairing identity (its public key goes into the
    // handoff, so call this before sending one) and opens the listeners.
    bool begin();
    void end();
    // The public key of the pairing identity, hex, valid after begin().
    const char* publicKeyHex() const;
    // The identity's pairing id (a UUID), valid after begin(): the Bonjour record's "pi".
    const char* pairingId() const;
    // The Vita's real Bluetooth address (most significant byte first), which /info
    // lists in bluetoothIDs and the tunnel's identification names. Until it is set
    // the AirPlay device id stands in.
    void setBluetoothAddress(const uint8_t address[6]);

    // Waits up to waitUs for socket activity and handles whatever happened.
    void service(uint32_t waitUs);

    bool phoneConnected() const;
    // The phone asked for the Bluetooth link to be dropped and the second iAP2
    // exchange (inside the AirPlay session) is authenticated, so nothing needs the
    // Bluetooth link any more.
    bool bluetoothNoLongerNeeded() const;

    struct Impl; // public only so the implementation file's helpers can name it

private:
    Impl* impl_;
};

} // namespace carplay
