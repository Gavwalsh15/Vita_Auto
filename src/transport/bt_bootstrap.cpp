#include "transport/bt_bootstrap.h"
#include "transport/wireless_status.h"

#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <cstring>

#include "vita_auto_usb.h"
#include "logger/vita_log.h"

namespace {

constexpr uint64_t HANDSHAKE_TIMEOUT_US = 20000000;
// Once the Wi-Fi details are sent, the phone may confirm or just connect.
constexpr uint64_t INFO_SETTLE_US = 6000000;
constexpr size_t READ_CHUNK = 512;
// After a failed send, wait before claiming the stream and sending again.
constexpr uint64_t SEND_RETRY_US = 3000000;
// ksceBtSendSpp has no Serial Port connection to write to: the phone is linked
// but has not opened the Android Auto channel (yet).
constexpr int BT_ERROR_NO_SPP_CONNECTION = static_cast<int>(0x802F0703u);
// Checking again is cheap, so look often while waiting for the channel.
constexpr uint64_t CHANNEL_RETRY_US = 1000000;
constexpr uint64_t RESEND_US = 2500000;
// After the phone has the Wi-Fi details, how long to wait for its TCP
// connection before starting the handshake over on the same link.
constexpr uint64_t WIFI_CONNECT_TIMEOUT_US = 30000000;

// Wireless Android Auto bootstrap messages: a 16-bit big-endian payload length
// and message type, then a protobuf payload. The ids, field numbers, enum
// values and the order (head unit sends WifiStartRequest first, the phone asks
// for WifiInfoRequest, the head unit answers with WifiInfoResponse) match the
// open-source WirelessAndroidAutoDongle project's .proto files and RFCOMM code.
// Still unconfirmed against a real phone: the BSSID text format. (The port is
// whatever WifiStartRequest says, so any free port works.)
constexpr uint16_t MSG_WIFI_START_REQUEST = 1;  // HU -> phone: ip, port
constexpr uint16_t MSG_WIFI_INFO_REQUEST = 2;   // phone -> HU
constexpr uint16_t MSG_WIFI_INFO_RESPONSE = 3;  // HU -> phone: ssid, key, bssid
constexpr uint16_t MSG_WIFI_CONNECT_STATUS = 6; // phone -> HU
constexpr uint16_t MSG_WIFI_START_RESPONSE = 7; // phone -> HU

// Status fields: WifiConnectStatus field 1, WifiStartResponse field 3.
// 0 is success; anything else means the phone gave up on the network.
constexpr uint32_t CONNECT_STATUS_FIELD = 1;
constexpr uint32_t START_RESPONSE_STATUS_FIELD = 3;

constexpr uint32_t SECURITY_OPEN = 1;
constexpr uint32_t SECURITY_WPA2_PERSONAL = 8;
constexpr uint32_t ACCESS_POINT_STATIC = 0;

void putVarint(std::vector<uint8_t>& out, uint32_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

void putStringField(std::vector<uint8_t>& out, uint32_t field, const char* text)
{
    const size_t length = std::strlen(text);
    putVarint(out, (field << 3) | 2);
    putVarint(out, static_cast<uint32_t>(length));
    out.insert(out.end(), text, text + length);
}

void putVarintField(std::vector<uint8_t>& out, uint32_t field, uint32_t value)
{
    putVarint(out, field << 3);
    putVarint(out, value);
}

// Reads a varint field from a protobuf payload as a signed 32-bit value (a
// negative int32 is sent as a 10-byte varint). False if the field is absent.
bool readInt32Field(const uint8_t* data, size_t size, uint32_t field, int32_t* out)
{
    size_t at = 0;
    auto readVarint = [&](uint64_t* value) {
        *value = 0;
        for (unsigned shift = 0; at < size && shift < 64; shift += 7) {
            const uint8_t byte = data[at++];
            *value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0)
                return true;
        }
        return false;
    };
    while (at < size) {
        uint64_t key = 0;
        if (!readVarint(&key))
            return false;
        const uint32_t wireType = static_cast<uint32_t>(key & 7);
        uint64_t value = 0;
        if (wireType == 0) {
            if (!readVarint(&value))
                return false;
            if ((key >> 3) == field) {
                *out = static_cast<int32_t>(static_cast<int64_t>(value));
                return true;
            }
        } else if (wireType == 2) {
            if (!readVarint(&value) || value > size - at)
                return false;
            at += static_cast<size_t>(value);
        } else if (wireType == 5) {
            at += 4;
        } else if (wireType == 1) {
            at += 8;
        } else {
            return false;
        }
    }
    return false;
}

// Names from Android Auto's status enum as used by the WirelessAndroidAutoDongle
// project; the number in the log is what counts if a name looks wrong. -10 and -11
// were confirmed against a phone's own log (PHONE_WIFI_DISABLED, WIFI_NETWORK_UNAVAILABLE).
const char* phoneStatusName(int32_t status)
{
    switch (status) {
    case 0: return "success";
    case -2: return "wifi inaccessible channel";
    case -3: return "wifi incorrect credentials";
    case -4: return "projection already started";
    case -5: return "wifi disabled";
    case -6: return "wifi not yet started";
    case -7: return "invalid host";
    case -8: return "no supported wifi partners";
    case -10: return "phone Wi-Fi is off";
    case -11: return "phone could not find the Wi-Fi network";
    default: return "unknown";
    }
}

void logBytes(const char* prefix, const uint8_t* data, size_t size)
{
    char hex[24 * 2 + 1];
    const size_t shown = size > 24 ? 24 : size;
    for (size_t i = 0; i < shown; i++)
        std::snprintf(hex + i * 2, 3, "%02x", data[i]);
    hex[shown * 2] = '\0';
    vitaLogInfo("bt-bootstrap: %s %u bytes: %s%s", prefix, static_cast<unsigned>(size),
            hex, size > shown ? "..." : "");
}

} // namespace

bool BtBootstrap::begin(const char* name, const char* recoverOriginal)
{
    end();
    if (!stack_.enable("bt-bootstrap", name, recoverOriginal, VITA_AUTO_BT_OPTION_RFCOMM)) {
        state_ = State::Failed;
        wireless_status::publish(Progress::Failed, -1);
        return false;
    }
    resetHandshake();
    retryAfterUs_ = 0;
    log_ = LogState{};
    state_ = State::WaitingForPhone;
    wireless_status::publish(Progress::Waiting);
    vitaLog("bt-bootstrap: enabled (Bluetooth name '%s')", name != nullptr ? name : "");
    return true;
}

void BtBootstrap::resetHandshake()
{
    doneAtUs_ = 0;
    rx_.clear();
    nextResendUs_ = 0;
    infoSent_ = false;
    handshakeStartUs_ = 0;
    infoSentUs_ = 0;
}

void BtBootstrap::handOff()
{
    // Unclaimed, the plugin's worker sleeps until a stack event instead of
    // polling the stream every few ms during the session.
    stack_.release();
    resetHandshake();
    wireless_status::publish(Progress::Off);
    state_ = State::Done;
    vitaLog("bt-bootstrap: phone on Wi-Fi; keeping Bluetooth up for the session");
}

void BtBootstrap::end()
{
    stack_.disable("bt-bootstrap");
    resetHandshake();
    // A failure stays on screen until the next attempt starts Bluetooth again.
    if (wireless_status::progress() != Progress::Failed)
        wireless_status::publish(Progress::Off);
    state_ = State::Idle;
}

bool BtBootstrap::sendMessage(uint16_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    frame.reserve(4 + payload.size());
    frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
    frame.push_back(static_cast<uint8_t>(payload.size()));
    frame.push_back(static_cast<uint8_t>(type >> 8));
    frame.push_back(static_cast<uint8_t>(type));
    frame.insert(frame.end(), payload.begin(), payload.end());
    // The info response carries the Wi-Fi password, and logs end up in bug
    // reports: show only its size.
    if (type == MSG_WIFI_INFO_RESPONSE)
        vitaLog("bt-bootstrap: send Wi-Fi details, %u bytes (not shown: has the password)",
                static_cast<unsigned>(frame.size()));
    else if (!log_.noChannel)
        logBytes("send", frame.data(), frame.size());
    const int written = vitaAutoBtWrite(frame.data(), static_cast<uint32_t>(frame.size()));
    lastWriteResult_ = written;
    if (written != static_cast<int>(frame.size())) {
        if (written != BT_ERROR_NO_SPP_CONNECTION)
            vitaLog("bt-bootstrap: write returned 0x%08x", static_cast<unsigned>(written));
        return false;
    }
    return true;
}

bool BtBootstrap::sendStartRequest(const WifiNetInfo& net, uint16_t port)
{
    std::vector<uint8_t> payload;
    putStringField(payload, 1, net.ip);
    putVarintField(payload, 2, port);
    return sendMessage(MSG_WIFI_START_REQUEST, payload);
}

bool BtBootstrap::sendInfoResponse(const WifiNetInfo& net, const char* password)
{
    if (net.ssid[0] == '\0')
        vitaLog("bt-bootstrap: Wi-Fi name unknown; the phone cannot join the network");
    if (password[0] == '\0')
        vitaLog("bt-bootstrap: no wifi_password in settings; telling the phone the "
                "network is open, which fails on a secured network");
    std::vector<uint8_t> payload;
    putStringField(payload, 1, net.ssid);
    putStringField(payload, 2, password);
    putStringField(payload, 3, net.bssid);
    putVarintField(payload, 4, password[0] != '\0' ? SECURITY_WPA2_PERSONAL : SECURITY_OPEN);
    putVarintField(payload, 5, ACCESS_POINT_STATIC);
    return sendMessage(MSG_WIFI_INFO_RESPONSE, payload);
}

void BtBootstrap::handleMessage(uint16_t type, const uint8_t* payload, size_t size,
                                const WifiNetInfo& net, const char* password)
{
    ++phoneMessages_;
    vitaLogInfo("bt-bootstrap: phone message type=%u size=%u", static_cast<unsigned>(type),
            static_cast<unsigned>(size));
    if (size > 0)
        logBytes("payload", payload, size);
    switch (type) {
    case MSG_WIFI_INFO_REQUEST:
        if (sendInfoResponse(net, password)) {
            infoSent_ = true;
            infoSentUs_ = sceKernelGetSystemTimeWide();
            wireless_status::publish(Progress::DetailsSent);
        }
        break;
    case MSG_WIFI_START_RESPONSE:
    case MSG_WIFI_CONNECT_STATUS: {
        int32_t status = 0;
        const uint32_t field = type == MSG_WIFI_CONNECT_STATUS
            ? CONNECT_STATUS_FIELD : START_RESPONSE_STATUS_FIELD;
        const bool hasStatus = readInt32Field(payload, size, field, &status);
        if (hasStatus)
            vitaLog("bt-bootstrap: phone %s status %d (%s)",
                    type == MSG_WIFI_CONNECT_STATUS ? "Wi-Fi connect" : "start response",
                    static_cast<int>(status), phoneStatusName(status));
        if (hasStatus && status != 0) {
            // The phone tried and gave up: no TCP connection is coming. Show
            // it rather than "waiting"; a new Bluetooth link starts over.
            state_ = State::Done;
            wireless_status::publish(Progress::PhoneRejected, status);
            break;
        }
        if (infoSent_) {
            state_ = State::Done;
            wireless_status::publish(Progress::WaitingForWifi);
            vitaLog("bt-bootstrap: phone acknowledged; waiting for its Wi-Fi connection");
        }
        break;
    }
    default:
        break; // Unknown types are logged above and otherwise ignored.
    }
}

void BtBootstrap::logDiscoverable(const VitaAutoBtInfo& info)
{
    if (info.discoverable != log_.discoverable) {
        log_.discoverable = info.discoverable;
        if (info.discoverable)
            vitaLog("bt-bootstrap: Vita is discoverable");
        else if (info.discoverableResult < 0)
            vitaLog("bt-bootstrap: not discoverable yet (0x%08x), retrying",
                    static_cast<unsigned>(info.discoverableResult));
    }
}

void BtBootstrap::trackLinkState(const VitaAutoBtInfo& info, uint64_t nowUs)
{
    if (!info.peerValid) {
        linkedSinceUs_ = 0;
        lastLinkState_ = -1;
        return;
    }
    const int linkState = vitaAutoBtConnectState(info.peerMac0, info.peerMac1);
    if (linkedSinceUs_ == 0)
        linkedSinceUs_ = nowUs;
    if (linkState != lastLinkState_) {
        vitaLogInfo("bt-state: connection state %d (%u s since the phone linked)", linkState,
                static_cast<unsigned>((nowUs - linkedSinceUs_) / 1000000));
        lastLinkState_ = linkState;
    }
}

// Gives up on the current handshake; the next pass claims the stream again.
void BtBootstrap::restartHandshake()
{
    stack_.release();
    resetHandshake();
    state_ = State::WaitingForPhone;
}


void BtBootstrap::beginHandshake(
    const WifiNetInfo& net, uint16_t port, uint64_t nowUs)
{
    if (net.ip[0] == '\0') {
        if (!log_.noWifi) {
            vitaLog("bt-bootstrap: phone linked but the Vita is not on Wi-Fi yet");
            log_.noWifi = true;
        }
        wireless_status::publish(Progress::NoWifi);
        return;
    }
    log_.noWifi = false;
    if (nowUs < retryAfterUs_)
        return;
    const int claim = stack_.claim();
    if (claim < 0) {
        vitaLog("bt-bootstrap: could not claim Bluetooth stream 0x%08x", claim);
        return;
    }
    resetHandshake();
    handshakeStartUs_ = nowUs;
    if (!log_.noChannel)
        vitaLog("bt-bootstrap: phone linked; telling it %s:%u (ssid '%s')",
                net.ip, static_cast<unsigned>(port), net.ssid);
    if (!sendStartRequest(net, port)) {
        stack_.release();
        if (lastWriteResult_ == BT_ERROR_NO_SPP_CONNECTION) {
            if (!log_.noChannel) {
                vitaLog("bt-bootstrap: the phone is linked but has not opened the "
                        "Android Auto channel; checking again every second");
                log_.noChannel = true;
            }
            retryAfterUs_ = nowUs + CHANNEL_RETRY_US;
            wireless_status::publish(Progress::NoChannel);
        } else {
            retryAfterUs_ = nowUs + SEND_RETRY_US;
            wireless_status::publish(Progress::SendFailed);
        }
        return;
    }
    log_.noChannel = false;
    nextResendUs_ = nowUs + RESEND_US;
    state_ = State::Handshaking;
    wireless_status::publish(Progress::Sending);
}

void BtBootstrap::receiveMessages(const WifiNetInfo& net, const char* password)
{
    uint8_t chunk[READ_CHUNK];
    for (;;) {
        const int got = vitaAutoBtReadWaitTimed(chunk, sizeof(chunk), 0);
        if (got <= 0)
            break;
        logBytes("recv", chunk, static_cast<size_t>(got));
        rx_.insert(rx_.end(), chunk, chunk + got);
    }
    while (rx_.size() >= 4) {
        const size_t length = (static_cast<size_t>(rx_[0]) << 8) | rx_[1];
        const uint16_t type = static_cast<uint16_t>((rx_[2] << 8) | rx_[3]);
        if (rx_.size() < 4 + length)
            break;
        handleMessage(type, rx_.data() + 4, length, net, password);
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(4 + length));
    }
}

// Resends the start request while the phone is silent, and ends the handshake
// when it has been answered long enough or taken too long.
void BtBootstrap::serviceHandshake(
    const WifiNetInfo& net, uint16_t port, uint64_t nowUs)
{
    if (state_ != State::Handshaking)
        return;
    if (!infoSent_ && phoneMessages_ == 0 && nowUs >= nextResendUs_) {
        // The phone has said nothing: send the start request again.
        nextResendUs_ = nowUs + RESEND_US;
        vitaLog("bt-bootstrap: no reply yet; sending the start request again");
        sendStartRequest(net, port);
    }
    if (state_ != State::Handshaking)
        return;
    if (infoSent_ && nowUs - infoSentUs_ >= INFO_SETTLE_US) {
        state_ = State::Done;
        wireless_status::publish(Progress::WaitingForWifi);
        vitaLog("bt-bootstrap: no acknowledgement; waiting for the phone anyway");
    } else if (nowUs - handshakeStartUs_ >= HANDSHAKE_TIMEOUT_US) {
        vitaLog("bt-bootstrap: handshake timed out; retrying");
        restartHandshake();
        wireless_status::publish(Progress::Waiting);
    }
}

void BtBootstrap::waitForPhoneWifi(uint64_t nowUs)
{
    if (state_ != State::Done)
        return;
    if (doneAtUs_ == 0) {
        doneAtUs_ = nowUs;
        return;
    }
    if (nowUs - doneAtUs_ < WIFI_CONNECT_TIMEOUT_US)
        return;
    vitaLog("bt-bootstrap: phone did not connect over Wi-Fi within %u s; starting over",
            static_cast<unsigned>(WIFI_CONNECT_TIMEOUT_US / 1000000));
    restartHandshake();
    retryAfterUs_ = nowUs + CHANNEL_RETRY_US;
    if (wireless_status::progress() != Progress::PhoneRejected)
        wireless_status::publish(Progress::Waiting);
}

BtBootstrap::State BtBootstrap::poll(
    const WifiNetInfo& net, uint16_t port, const char* password)
{
    if (!stack_.enabled() || state_ == State::Failed || state_ == State::Idle)
        return state_;

    VitaAutoBtInfo info{};
    const BtStack::Status status = stack_.poll(info);
    if (status == BtStack::Status::Failed) {
        state_ = State::Failed;
        return state_;
    }
    if (status == BtStack::Status::Starting)
        return state_;
    logDiscoverable(info);

    const uint64_t nowUs = sceKernelGetSystemTimeWide();

    trackLinkState(info, nowUs);

    if (!info.peerValid) {
        if (stack_.claimed() || state_ != State::WaitingForPhone)
            restartHandshake();
        if (wireless_status::progress() != Progress::SendFailed)
            wireless_status::publish(Progress::Waiting);
        return state_;
    }
    if (state_ == State::WaitingForPhone) {
        beginHandshake(net, port, nowUs);
        return state_;
    }
    if (!stack_.claimed())
        return state_;

    receiveMessages(net, password);
    serviceHandshake(net, port, nowUs);
    waitForPhoneWifi(nowUs);
    return state_;
}
