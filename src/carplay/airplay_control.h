#pragma once

#include "airplay.h"

namespace carplay {

constexpr uint32_t TOUCH_CONTACTS = 2;
// The receiver's AirPlay feature bits: /info's "features" and the Bonjour record's.
constexpr int64_t CARPLAY_FEATURES = 0x615653aee2ll;
// HID device ids as the /info plist names them and hidSendReport addresses them.
extern const char HID_TOUCH[];
extern const char HID_KNOB[];
extern const char HID_MEDIA[];
extern const char HID_TELEPHONY[];

// What this receiver tells the phone about itself in /info.
struct InfoConfig {
    const char* name = "Vita Auto";
    const char* model = "VitaAuto";
    const char* manufacturer = "Vita Auto";
    const char* deviceId = "";
    const char* bluetoothMac = "";
    const char* sourceVersion = "366.0";
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t widthMm = 110; // The Vita's 5 inch panel
    uint32_t maxFps = 30;
};

nbtplay::Plist buildInfo(const InfoConfig& config);

// A compact one-line text of a plist (keys and scalar values, cut off at cap) and
// a log line for a request body that holds one. For finding out what the phone asks.
void describePlist(const nbtplay::Plist& value, char* out, size_t cap);
void logPlistBody(const char* what, const uint8_t* data, size_t size);

// The phone's iAP2 data stream (type 130): where the second iAP2 exchange runs.
class TunnelHost {
public:
    virtual ~TunnelHost() = default;
    // seed is the stream's "seed"; fills the TCP port the phone should connect to.
    virtual bool openTunnel(uint64_t seed, uint32_t& port) = 0;
    virtual void closeTunnel() = 0;
    // A UDP port for the phone's low-power keep-alive datagrams (nothing is read
    // from them); fills the port to report in the session SETUP reply.
    virtual bool openKeepAlive(uint32_t& port) = 0;
};

// nbtplay's AirPlayControl plus: touch screen in /info, PCM/AAC-LC audio, and the
// iAP2 data stream in SETUP.
class Control {
public:
    Control(const InfoConfig& config, nbtplay::AirPlaySecurity& security,
            nbtplay::AirPlayMedia& media, TunnelHost& tunnel)
        : config_(config), security_(security), media_(media), tunnel_(tunnel) {}

    // Bytes from the control connection in, response bytes appended to out.
    // False: close the connection (protocol error).
    bool feed(const uint8_t* data, size_t length, nbtplay::Bytes& out);

    unsigned requests() const { return requests_; }
    const char* lastPath() const { return lastPath_; }
    // The phone sent "disableBluetooth": it considers the Wi-Fi session the link now
    // and expects the accessory to drop its Bluetooth connection to it.
    bool bluetoothDisableRequested() const { return bluetoothDisableRequested_; }

private:
    int handle(const nbtplay::RtspRequest& request, nbtplay::Bytes& body, const char*& contentType);
    int handleSetup(const nbtplay::RtspRequest& request, nbtplay::Bytes& body);
    int handleTeardown(const nbtplay::RtspRequest& request);
    void markStream(int type, bool open);
    bool streamOpen(int type) const;

    InfoConfig config_;
    nbtplay::AirPlaySecurity& security_;
    nbtplay::AirPlayMedia& media_;
    TunnelHost& tunnel_;
    nbtplay::Bytes pending_;   // plaintext not yet framed
    nbtplay::Bytes encrypted_; // ciphertext not yet decrypted
    bool tunnelOpen_ = false;
    bool bluetoothDisableRequested_ = false;
    unsigned requests_ = 0;
    uint32_t openStreams_ = 0;
    char lastPath_[64] = {};
};

} // namespace carplay
