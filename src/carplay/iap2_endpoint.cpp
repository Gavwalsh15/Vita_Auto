#include "carplay/iap2_endpoint.h"

#include <cstdio>

#include "logger/vita_log.h"

#include "auth.h"
#include "iap2_auth.h"
#include "iap2_csm.h"
#include "iap2_link.h"

namespace carplay {
namespace {

constexpr uint16_t MSG_START_IDENTIFICATION = 0x1D00;
constexpr uint16_t MSG_IDENTIFICATION_INFORMATION = 0x1D01;
constexpr uint16_t MSG_IDENTIFICATION_ACCEPTED = 0x1D02;
constexpr uint16_t MSG_IDENTIFICATION_REJECTED = 0x1D03;
constexpr uint16_t MSG_CARPLAY_AVAILABILITY = 0x4300;
constexpr uint16_t MSG_CARPLAY_START_SESSION = 0x4301;
constexpr uint16_t MSG_REQUEST_WIFI_CONFIG = 0x5702;
constexpr uint16_t MSG_WIFI_CONFIG = 0x5703;

const char* messageName(uint16_t id)
{
    switch (id) {
    case 0x1D00: return "StartIdentification";
    case 0x1D01: return "IdentificationInformation";
    case 0x1D02: return "IdentificationAccepted";
    case 0x1D03: return "IdentificationRejected";
    case 0xAA00: return "RequestAuthenticationCertificate";
    case 0xAA01: return "AuthenticationCertificate";
    case 0xAA02: return "RequestAuthenticationChallengeResponse";
    case 0xAA03: return "AuthenticationResponse";
    case 0xAA04: return "AuthenticationFailed";
    case 0xAA05: return "AuthenticationSucceeded";
    case 0x4300: return "CarPlayAvailability";
    case 0x4301: return "CarPlayStartSession";
    case 0x4E0D: return "WirelessCarPlayUpdate";
    case 0x4E0E: return "WirelessCarPlayUpdate2";
    case 0x5702: return "RequestAccessoryWiFiConfigurationInformation";
    case 0x5703: return "AccessoryWiFiConfigurationInformation";
    case 0x5001: return "NowPlayingUpdate";
    case 0x5201: return "RouteGuidanceUpdate";
    case 0xAE01: return "PowerUpdate";
    case 0xEA00: return "StartExternalAccessoryProtocolSession";
    case 0xEA01: return "StopExternalAccessoryProtocolSession";
    default: return "?";
    }
}

// What the accessory advertises for wireless: no power source messages, the Bluetooth
// and Wireless CarPlay transport components, and the wireless handoff messages both ways.
const uint16_t SENT_MESSAGES[] = {
    0xAA01, 0xAA03, 0x5000, 0x5002, 0x5200, 0x5203, 0xAE00, 0xAE02,
    0x4157, 0x4159, 0x4154, 0x4156, 0x4301, 0x5703,
};
const uint16_t RECEIVED_MESSAGES[] = {
    0xAA00, 0xAA02, 0xAA04, 0xAA05, 0xEA00, 0xEA01, 0x5001, 0x5201, 0x5202,
    0xAE01, 0x4158, 0x4155, 0x4300, 0x4E0D, 0x4E0E, 0x5702,
};

bool buildIdentification(const Handoff& h, nbtplay::Bytes& frame)
{
    using namespace nbtplay;
    char serial[32];
    std::snprintf(serial, sizeof(serial), "VITAAUTO%02X%02X%02X", h.mac[3], h.mac[4], h.mac[5]);
    CsmBuilder b(MSG_IDENTIFICATION_INFORMATION);
    b.add_string(0, h.name);
    b.add_string(1, "VitaAuto");
    b.add_string(2, "Vita Auto");
    b.add_string(3, serial);
    b.add_string(4, "1.0.0");
    b.add_string(5, "1.0");
    b.add_u16_list(6, SENT_MESSAGES, sizeof(SENT_MESSAGES) / sizeof(SENT_MESSAGES[0]));
    b.add_u16_list(7, RECEIVED_MESSAGES, sizeof(RECEIVED_MESSAGES) / sizeof(RECEIVED_MESSAGES[0]));
    b.add_u8(8, 0);   // The phone is not powered by the accessory.
    b.add_u16(9, 20); // MaximumCurrentDrawnFromDevice

    CsmGroup ea; // SupportedExternalAccessoryProtocol
    ea.add_u8(0, 1);
    ea.add_string(1, "com.vitaauto.carplay");
    ea.add_u8(2, 0);
    b.add_group(10, ea);

    b.add_string(12, "en");
    b.add_string(13, "en");

    CsmGroup bluetooth; // BluetoothTransportComponent
    bluetooth.add_u16(0, 0);
    bluetooth.add_string(1, "blue");
    bluetooth.add_void(2);
    bluetooth.add_bytes(3, h.btAddressKnown ? h.btAddress : h.mac, 6);
    bluetooth.add_string(4, "blue");
    bluetooth.add_void(5);
    b.add_group(17, bluetooth);

    CsmGroup wireless; // WirelessCarPlayTransportComponent
    wireless.add_u16(0, 1);
    wireless.add_string(1, h.ssid);
    wireless.add_void(2);
    wireless.add_u16(3, 1);
    wireless.add_void(4);
    wireless.add_void(5);
    b.add_group(24, wireless);

    CsmGroup route; // RouteGuidanceDisplayComponent
    route.add_u16(0, 42);
    route.add_string(1, "RouteGuidance");
    route.add_u16(2, 64);
    route.add_u16(4, 64);
    route.add_u16(6, 8);
    b.add_group(30, route);

    return b.finish(frame);
}

bool buildWifiConfig(const Handoff& h, nbtplay::Bytes& frame)
{
    nbtplay::CsmBuilder b(MSG_WIFI_CONFIG);
    // Parameter 0 (BSSID) is left out: the app does not know the AP's BSSID.
    b.add_string(1, h.ssid);
    b.add_string(2, h.passphrase);
    b.add_u8(3, h.security);
    b.add_u8(4, h.channel);
    return b.finish(frame);
}

bool buildStartSession(const Handoff& h, nbtplay::Bytes& frame)
{
    using namespace nbtplay;
    CsmBuilder b(MSG_CARPLAY_START_SESSION);
    CsmGroup wireless;
    wireless.add_string(0, h.ssid);
    wireless.add_string(1, h.passphrase);
    wireless.add_u8(2, h.channel);
    wireless.add_string(3, h.ip);
    wireless.add_u8(4, h.security);
    b.add_group(1, wireless);
    b.add_u32(2, h.airplayPort);
    b.add_string(3, h.deviceId);
    b.add_string(4, h.publicKeyHex);
    b.add_string(5, h.sourceVersion);
    return b.finish(frame);
}

bool buildSubscription(int index, nbtplay::Bytes& frame)
{
    using namespace nbtplay;
    switch (index) {
    case 0: { // StartNowPlayingUpdates
        CsmBuilder b(0x5000);
        CsmGroup properties;
        for (uint16_t id : {1, 4, 6, 12, 26})
            properties.add_void(id);
        b.add_group(0, properties);
        CsmGroup playback;
        for (uint16_t id : {0, 1, 7})
            playback.add_void(id);
        b.add_group(1, playback);
        return b.finish(frame);
    }
    case 1: { // StartRouteGuidanceUpdates
        CsmBuilder b(0x5200);
        b.add_u16(0, 42);
        b.add_void(1);
        b.add_void(2);
        return b.finish(frame);
    }
    case 2: { // StartPowerUpdates
        CsmBuilder b(0xAE00);
        b.add_void(4);
        b.add_void(5);
        b.add_void(6);
        return b.finish(frame);
    }
    case 3: { // StartCommunicationsUpdates
        CsmBuilder b(0x4157);
        b.add_void(0);
        b.add_void(4);
        b.add_void(5);
        return b.finish(frame);
    }
    case 4: { // StartCallStateUpdates
        CsmBuilder b(0x4154);
        for (uint16_t id : {0, 1, 2, 3, 4, 11})
            b.add_void(id);
        return b.finish(frame);
    }
    default:
        return false;
    }
}

} // namespace

struct Iap2Endpoint::Impl {
    Impl(const Handoff& h, nbtplay::Authenticator& a, Iap2Role r)
        : handoff(h), role(r), auth(a) {}

    void onFrame(const nbtplay::CsmFrame& frame, int64_t nowMs);
    void send(const nbtplay::Bytes& frames, const char* what, int64_t nowMs);
    void drain(int64_t nowMs);

    Handoff handoff;
    Iap2Role role;
    nbtplay::Authenticator& auth;
    nbtplay::Iap2Link* link = nullptr;
    nbtplay::CsmParser parser;
    nbtplay::Iap2AuthExchange auth_{auth};
    bool identSent = false;
    bool authDone = false;
    bool sent = false;
    bool authLogged = false;
    uint32_t messages = 0;
    nbtplay::Iap2Link::State lastState = nbtplay::Iap2Link::State::Idle;
};

void Iap2Endpoint::Impl::send(const nbtplay::Bytes& frames, const char* what, int64_t nowMs)
{
    if (link == nullptr || frames.empty())
        return;
    if (!link->send_control(frames.data(), frames.size(), nowMs))
        vitaLog("carplay-iap2: could not queue %s (%u bytes)", what,
                static_cast<unsigned>(frames.size()));
}

void Iap2Endpoint::Impl::onFrame(const nbtplay::CsmFrame& frame, int64_t nowMs)
{
    ++messages;
    const char* tag = role == Iap2Role::Bluetooth ? "bt" : "tunnel";
    if (VITA_AUTO_VERBOSE_LOGGING)
        vitaLog("carplay-iap2[%s]: phone message 0x%04x %s (%u bytes)", tag,
                static_cast<unsigned>(frame.message_id), messageName(frame.message_id),
                static_cast<unsigned>(frame.body.size()));

    nbtplay::Bytes reply;
    switch (frame.message_id) {
    case MSG_START_IDENTIFICATION:
        if (identSent)
            break;
        if (buildIdentification(handoff, reply)) {
            send(reply, "identification", nowMs);
            identSent = true;
        } else {
            vitaLog("carplay-iap2[%s]: could not build the identification", tag);
        }
        break;
    case MSG_IDENTIFICATION_ACCEPTED:
        vitaLog("carplay-iap2[%s]: identification accepted", tag);
        break;
    case MSG_IDENTIFICATION_REJECTED: {
        size_t cursor = 0;
        nbtplay::CsmParam param;
        while (frame.next_param(cursor, param))
            vitaLog("carplay-iap2[%s]: rejected parameter id=%u length=%u", tag,
                    static_cast<unsigned>(param.id), static_cast<unsigned>(param.length));
        break;
    }
    case nbtplay::kMsgRequestCertificate:
    case nbtplay::kMsgRequestChallengeResponse:
    case nbtplay::kMsgAuthenticationSucceeded:
    case nbtplay::kMsgAuthenticationFailed: {
        const auto status = auth_.on_frame(frame, reply);
        send(reply, "authentication reply", nowMs);
        if (status == nbtplay::Iap2AuthExchange::Status::Failed && !authLogged) {
            authLogged = true;
            vitaLog("carplay-iap2[%s]: authentication failed: %s (see docs/carplay.md: the "
                    "accessory identity is supplied by you)", tag,
                    auth_.failure() != nullptr ? auth_.failure() : "failed");
        } else if (status == nbtplay::Iap2AuthExchange::Status::Succeeded && !authDone) {
            authDone = true;
            vitaLog("carplay-iap2[%s]: *** the phone accepted the authentication ***", tag);
            if (role == Iap2Role::Tunnel) {
                for (int i = 0; i < 5; ++i) {
                    nbtplay::Bytes subscription;
                    if (buildSubscription(i, subscription))
                        send(subscription, "subscription", nowMs);
                }
                vitaLog("carplay-iap2[tunnel]: subscribed to now-playing, route, power, "
                        "communications and call state updates");
            }
        }
        break;
    }
    case MSG_REQUEST_WIFI_CONFIG:
        if (buildWifiConfig(handoff, reply)) {
            send(reply, "wifi configuration", nowMs);
            vitaLog("carplay-iap2[%s]: sent the Wi-Fi network '%s'", tag, handoff.ssid);
        }
        break;
    case MSG_CARPLAY_AVAILABILITY:
        if (buildStartSession(handoff, reply)) {
            send(reply, "start session", nowMs);
            sent = true;
            vitaLog("carplay-iap2[%s]: sent CarPlayStartSession %s:%u", tag, handoff.ip,
                    static_cast<unsigned>(handoff.airplayPort));
        }
        break;
    default:
        break; // Everything else is only logged above.
    }
}

void Iap2Endpoint::Impl::drain(int64_t nowMs)
{
    nbtplay::Iap2Event event;
    while (link != nullptr && link->poll_event(event)) {
        switch (event.type) {
        case nbtplay::Iap2Event::Type::Control: {
            parser.offer(event.data.data(), event.data.size());
            nbtplay::CsmFrame frame;
            while (parser.next(frame))
                onFrame(frame, nowMs);
            break;
        }
        case nbtplay::Iap2Event::Type::Dead:
            vitaLog("carplay-iap2: link ended: %s", event.reason != nullptr ? event.reason : "stream ended");
            break;
        default:
            break;
        }
    }
    if (link != nullptr && link->state() != lastState) {
        lastState = link->state();
        vitaLog("carplay-iap2: link state %d", static_cast<int>(lastState));
    }
}

Iap2Endpoint::Iap2Endpoint(const Handoff& handoff, nbtplay::Authenticator& authenticator, Iap2Role role)
    : impl_(new Impl(handoff, authenticator, role))
{
}

Iap2Endpoint::~Iap2Endpoint()
{
    nbtplay::Iap2Link::destroy(impl_->link);
    delete impl_;
}

bool Iap2Endpoint::start(int64_t nowMs)
{
    nbtplay::Iap2LinkConfig config;
    config.max_outgoing = 4;
    config.control_session_version = 2;
    config.acknowledgement_timeout_ms = 500;
    config.zero_acknowledgements = impl_->role == Iap2Role::Tunnel;
    impl_->link = nbtplay::Iap2Link::create(config);
    if (impl_->link == nullptr)
        return false;
    // The phone opens the Bluetooth link; on the tunnel the accessory does.
    impl_->link->start(impl_->role == Iap2Role::Tunnel, nowMs);
    impl_->lastState = impl_->link->state();
    return true;
}

void Iap2Endpoint::feed(const uint8_t* data, size_t size, int64_t nowMs)
{
    if (impl_->link == nullptr)
        return;
    impl_->link->feed(data, size, nowMs);
    impl_->drain(nowMs);
}

void Iap2Endpoint::tick(int64_t nowMs)
{
    if (impl_->link == nullptr)
        return;
    impl_->link->advance_time(nowMs);
    impl_->drain(nowMs);
}

void Iap2Endpoint::takeOutput(std::vector<uint8_t>& out)
{
    if (impl_->link == nullptr)
        return;
    size_t length = 0;
    const uint8_t* data = impl_->link->output(length);
    if (length > 0) {
        out.insert(out.end(), data, data + length);
        impl_->link->consume_output();
    }
}

bool Iap2Endpoint::authSucceeded() const { return impl_->authDone; }
bool Iap2Endpoint::handoffSent() const { return impl_->sent; }
uint32_t Iap2Endpoint::messages() const { return impl_->messages; }

} // namespace carplay
