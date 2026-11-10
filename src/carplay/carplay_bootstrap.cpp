#include "carplay/carplay_bootstrap.h"

#include "logger/vita_log.h"

#include <cstdio>
#include <cstring>

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>

#include <memory>
#include <vector>

#include "carplay/airplay_control.h"
#include "carplay/bonjour.h"
#include "carplay/carplay_session.h"
#include "carplay/iap2_endpoint.h"
#include "carplay/mfi_authenticator.h"
#include "transport/bt_stack.h"
#include "transport/phone_network.h"
#include "transport/tcp_transport.h"
#include "transport/wireless_status.h"
#include "vita_auto_usb.h"

namespace carplay {
namespace {

constexpr size_t READ_CHUNK = 512;
constexpr uint32_t PIPE_READ_WAIT_US = 2000;
constexpr uint32_t SESSION_WAIT_US = 2000;
constexpr uint32_t IDLE_WAIT_US = 20000;
constexpr uint64_t BT_DROP_GRACE_US = 2000000;
constexpr uint16_t AIRPLAY_PORT = 7000;

int64_t nowMs() { return static_cast<int64_t>(sceKernelGetSystemTimeWide() / 1000); }

class Orchestrator {
public:
    explicit Orchestrator(const Config& config) : config_(config) {}
    ~Orchestrator() { end(); }

    bool begin(bool (*cancelled)());
    void end();
    void run(bool (*cancelled)());
    bool accessPointFailed() const { return apFailed_; }

private:
    bool bringUpNetwork(bool (*cancelled)());
    void fillHandoff();
    void serviceBluetooth(const VitaAutoBtInfo& info);
    void dropBluetoothWhenDone(const VitaAutoBtInfo& info, uint64_t nowUs);
    void startEndpoint();
    void dropEndpoint(const char* why);
    void pumpEndpoint();
    void logMilestones(const VitaAutoBtInfo& info);

    Config config_;
    bool netHeld_ = false;
    ::BtStack stack_;
    bool btAddressSet_ = false;
    bool btAddressWarned_ = false;
    // Dropping the Bluetooth link once the phone has asked for it (disableBluetooth)
    // and the tunnel is up: 0 = not armed, otherwise when the grace period began.
    uint64_t btDropArmedUs_ = 0;
    bool btDropped_ = false;
    uint32_t lastMilestones_ = 0;

    MfiAuthenticator authenticator_;
    PhoneNetwork network_;
    bool apFailed_ = false;
    Handoff handoff_;
    std::unique_ptr<Session> session_;
    Bonjour bonjour_;

    std::unique_ptr<Iap2Endpoint> endpoint_;
    std::vector<uint8_t> tx_;
    uint32_t rxBytes_ = 0;
    bool handoffLogged_ = false;
};

// The network the phone is told to join. Our own access point when the Vita has
// no network of its own and the setting allows; the router it is on otherwise.
bool Orchestrator::bringUpNetwork(bool (*cancelled)())
{
    const int stack = net_stack::acquire();
    if (stack < 0) {
        vitaLog("carplay: network stack failed 0x%08x", static_cast<unsigned>(stack));
        return false;
    }
    netHeld_ = true;

    network_.begin(config_.wireless, "carplay");
    while (network_.starting()) {
        if (cancelled())
            return false;
        sceKernelDelayThread(20000);
        network_.poll();
    }
    PhoneNetwork::Info net;
    if (!network_.current(net)) {
        apFailed_ = network_.accessPointFailed();
        if (!apFailed_)
            vitaLog("carplay: the Vita is not on Wi-Fi and its access point is off; nothing to hand the phone");
        return false;
    }
    std::snprintf(handoff_.ip, sizeof(handoff_.ip), "%s", net.ip);
    std::snprintf(handoff_.ssid, sizeof(handoff_.ssid), "%s", net.ssid);
    std::snprintf(handoff_.passphrase, sizeof(handoff_.passphrase), "%s", net.password);
    handoff_.security = handoff_.passphrase[0] != '\0' ? 2 : 0;
    handoff_.channel = static_cast<uint8_t>(net.channel);
    return true;
}

void Orchestrator::fillHandoff()
{
    std::snprintf(handoff_.name, sizeof(handoff_.name), "%s", config_.wireless.bluetoothName);
    handoff_.airplayPort = AIRPLAY_PORT;
    // The Wi-Fi address is the AirPlay device id. The Bluetooth address is a
    // different one, filled in from the module once the stack reports it (run()).
    SceNetEtherAddr mac{};
    if (sceNetGetMacAddress(&mac, 0) >= 0)
        std::memcpy(handoff_.mac, mac.data, sizeof(handoff_.mac));
    formatMac(handoff_.deviceId, handoff_.mac, true);
}

bool Orchestrator::begin(bool (*cancelled)())
{
    end();
    wireless_status::publish(wireless_status::Progress::Waiting);

    if (authenticator_.load(config_.dataDirectory))
        vitaLog("carplay: %s", authenticator_.status());
    else
        vitaLog("carplay: no accessory identity: %s. The phone will stop at its authentication "
                "request (see docs/carplay.md)", authenticator_.status());

    // Bluetooth first, then the access point: the Wi-Fi/Bluetooth chip is shared,
    // and this is the order the Android Auto wireless flow raised the AP in.
    // The iAP2 client needs the RFCOMM fix and the service search patch, which
    // the plugin applies for these options.
    const uint32_t btOptions = VITA_AUTO_BT_OPTION_RFCOMM | VITA_AUTO_BT_OPTION_IAP2_CLIENT;
    if (!stack_.enable("carplay", config_.wireless.bluetoothName, config_.wireless.recoverDeviceName, btOptions) ||
        !bringUpNetwork(cancelled)) {
        wireless_status::publish(wireless_status::Progress::Failed, -1);
        return false;
    }
    fillHandoff();

    SessionConfig sessionConfig;
    sessionConfig.handoff = handoff_;
    sessionConfig.audioOffer = config_.audioOffer;
    std::snprintf(sessionConfig.statePath, sizeof(sessionConfig.statePath), "%s/pairing.bin",
                  config_.dataDirectory);
    session_.reset(new Session(sessionConfig, authenticator_));
    if (!session_->begin()) {
        vitaLog("carplay: could not start the AirPlay receiver");
        wireless_status::publish(wireless_status::Progress::Failed, -2);
        return false;
    }
    std::snprintf(handoff_.publicKeyHex, sizeof(handoff_.publicKeyHex), "%s", session_->publicKeyHex());
    Bonjour::Service service;
    std::snprintf(service.name, sizeof(service.name), "%s", handoff_.name);
    std::snprintf(service.model, sizeof(service.model), "%s", InfoConfig().model);
    std::snprintf(service.deviceId, sizeof(service.deviceId), "%s", handoff_.deviceId);
    std::snprintf(service.sourceVersion, sizeof(service.sourceVersion), "%s", handoff_.sourceVersion);
    std::snprintf(service.pairingId, sizeof(service.pairingId), "%s", session_->pairingId());
    std::snprintf(service.publicKey, sizeof(service.publicKey), "%s", handoff_.publicKeyHex);
    service.features = CARPLAY_FEATURES;
    service.port = handoff_.airplayPort;
    if (!bonjour_.begin(handoff_.ip, service))
        vitaLog("carplay: Bonjour is off: the phone's control service will not be looked for");

    vitaLog("carplay: Bluetooth enabled; waiting for a paired iPhone to connect");
    return true;
}

void Orchestrator::end()
{
    bonjour_.end();
    dropEndpoint(nullptr);
    if (session_) {
        session_->end();
        session_.reset();
    }
    network_.end();
    stack_.disable("carplay");
    if (netHeld_) {
        net_stack::release();
        netHeld_ = false;
    }
    if (wireless_status::progress() != wireless_status::Progress::Failed)
        wireless_status::publish(wireless_status::Progress::Off);
}

// One line per step the link makes: what is new, and which RFCOMM channel the SDP
// search named (the phone refuses the data channel when that is wrong).
void Orchestrator::logMilestones(const VitaAutoBtInfo& info)
{
    static const struct { uint32_t bit; const char* name; } STEPS[] = {
        {VITA_AUTO_IAP2_CONN, "found"}, {VITA_AUTO_IAP2_L2CAP_RSP, "l2cap"},
        {VITA_AUTO_IAP2_CFG_REQ, "cfg-req"}, {VITA_AUTO_IAP2_CFG_RSP, "cfg-rsp"},
        {VITA_AUTO_IAP2_SABM0, "mux-req"}, {VITA_AUTO_IAP2_UA0, "mux-up"},
        {VITA_AUTO_IAP2_SDP_SENT, "sdp-sent"}, {VITA_AUTO_IAP2_SDP_DONE, "sdp-done"},
        {VITA_AUTO_IAP2_SABM_DLC, "dlc-req"}, {VITA_AUTO_IAP2_UA_DLC, "dlc-up"},
        {VITA_AUTO_IAP2_MSC_SENT, "msc-sent"}, {VITA_AUTO_IAP2_MSC_RSP, "msc-rsp"},
        {VITA_AUTO_IAP2_PIPE, "pipe"}, {VITA_AUTO_IAP2_DM, "DM (refused)"},
        {VITA_AUTO_IAP2_LOST, "LOST (phone closed)"}, {VITA_AUTO_IAP2_SDP_CFG_REQ, "sdp-cfg-req"},
        {VITA_AUTO_IAP2_SDP_CFG_RSP, "sdp-cfg-rsp"},
    };
    const uint32_t milestones = info.iap2Milestones;
    if (milestones == lastMilestones_)
        return;
    char added[160] = {};
    size_t at = 0;
    for (const auto& step : STEPS) {
        if ((milestones & step.bit) != 0 && (lastMilestones_ & step.bit) == 0 && at < sizeof(added))
            at += static_cast<size_t>(std::snprintf(added + at, sizeof(added) - at, "%s%s",
                                                    at != 0 ? ", " : "", step.name));
    }
    vitaLog("carplay: link steps 0x%05x: %s (rfcomm channel %u, open result 0x%08x)",
            static_cast<unsigned>(milestones), added, static_cast<unsigned>(info.iap2Channel),
            static_cast<unsigned>(info.iap2OpenResult));
    lastMilestones_ = milestones;
}

// A fresh iAP2 endpoint for each new pipe. The phone starts the link
// negotiation; the accessory side only answers.
void Orchestrator::startEndpoint()
{
    dropEndpoint(nullptr);
    endpoint_.reset(new Iap2Endpoint(handoff_, authenticator_, Iap2Role::Bluetooth));
    if (!endpoint_->start(nowMs())) {
        vitaLog("carplay: could not start the iAP2 endpoint");
        endpoint_.reset();
        return;
    }
    rxBytes_ = 0;
    handoffLogged_ = false;
    vitaLog("carplay: iAP2 pipe up; link started (accessory side)");
}

void Orchestrator::dropEndpoint(const char* why)
{
    if (!endpoint_)
        return;
    if (why != nullptr)
        vitaLog("carplay: iAP2 pipe closed (%s): %u B received, %u messages, auth %s, handoff %s", why,
                static_cast<unsigned>(rxBytes_), static_cast<unsigned>(endpoint_->messages()),
                endpoint_->authSucceeded() ? "ok" : "no", endpoint_->handoffSent() ? "sent" : "not sent");
    endpoint_.reset();
    tx_.clear();
}

void Orchestrator::pumpEndpoint()
{
    if (!endpoint_)
        return;
    uint8_t chunk[READ_CHUNK];
    // The first read sleeps until data arrives or the timeout passes; the rest
    // only take what is already queued.
    uint32_t waitUs = PIPE_READ_WAIT_US;
    for (;;) {
        const int got = vitaAutoBtReadWaitTimed(chunk, sizeof(chunk), waitUs);
        waitUs = 0;
        if (got <= 0)
            break;
        rxBytes_ += static_cast<uint32_t>(got);
        endpoint_->feed(chunk, static_cast<size_t>(got), nowMs());
    }
    endpoint_->tick(nowMs());
    endpoint_->takeOutput(tx_);

    size_t offset = 0;
    while (offset < tx_.size()) {
        const int written =
            vitaAutoBtWrite(tx_.data() + offset, static_cast<uint32_t>(tx_.size() - offset));
        if (written <= 0)
            break;
        offset += static_cast<size_t>(written);
    }
    if (offset > 0)
        tx_.erase(tx_.begin(), tx_.begin() + static_cast<std::ptrdiff_t>(offset));

    if (!handoffLogged_ && endpoint_->handoffSent()) {
        handoffLogged_ = true;
        vitaLog("carplay: wireless handoff sent; waiting for the phone on %s:%u", handoff_.ip,
                static_cast<unsigned>(handoff_.airplayPort));
        bonjour_.arm(sceKernelGetSystemTimeWide());
    }
}

void Orchestrator::dropBluetoothWhenDone(const VitaAutoBtInfo& info, uint64_t nowUs)
{
    if (!session_->bluetoothNoLongerNeeded()) {
        btDropArmedUs_ = 0;
        btDropped_ = false;
        return;
    }
    if (btDropped_ || !info.peerValid)
        return;
    if (btDropArmedUs_ == 0) {
        btDropArmedUs_ = nowUs;
        return;
    }
    if (nowUs - btDropArmedUs_ < BT_DROP_GRACE_US)
        return;
    btDropped_ = true;
    const int result = vitaAutoBtDisconnect(info.peerMac0, info.peerMac1);
    vitaLog("carplay: phone asked to disable Bluetooth and the tunnel is up; dropping the link (result 0x%08x)",
            static_cast<unsigned>(result));
}

void Orchestrator::serviceBluetooth(const VitaAutoBtInfo& info)
{
    const bool pipeNow = (info.iap2Milestones & VITA_AUTO_IAP2_PIPE) != 0 && info.peerValid != 0;
    if (pipeNow && !endpoint_)
        startEndpoint();
    else if (!pipeNow && endpoint_)
        dropEndpoint("link ended");
    pumpEndpoint();
}

void Orchestrator::run(bool (*cancelled)())
{
    while (!cancelled()) {
        VitaAutoBtInfo info{};
        const BtStack::Status status = stack_.poll(info);
        if (status == BtStack::Status::Failed)
            return;
        if (status == BtStack::Status::Starting) {
            session_->service(IDLE_WAIT_US);
            continue;
        }
        // The phone ties CarPlay's audio route to the Bluetooth address it is linked
        // with, so the identification and /info have to carry the Vita's real one,
        // not the Wi-Fi address that stood in for it before.
        if (!btAddressSet_ && (info.iap2LocalMac0 != 0 || info.iap2LocalMac1 != 0)) {
            const uint32_t low = info.iap2LocalMac0, high = info.iap2LocalMac1;
            const uint8_t address[6] = {
                static_cast<uint8_t>(high >> 8), static_cast<uint8_t>(high),
                static_cast<uint8_t>(low >> 24), static_cast<uint8_t>(low >> 16),
                static_cast<uint8_t>(low >> 8),  static_cast<uint8_t>(low),
            };
            std::memcpy(handoff_.btAddress, address, sizeof(address));
            handoff_.btAddressKnown = true;
            session_->setBluetoothAddress(address);
            btAddressSet_ = true;
            char text[18];
            formatMac(text, address, true);
            vitaLog("carplay: Vita Bluetooth address %s (Wi-Fi %s)", text, handoff_.deviceId);
        } else if (!btAddressSet_ && !btAddressWarned_ && info.iap2Installed) {
            btAddressWarned_ = true;
            vitaLog("carplay: Vita Bluetooth address not reported; the Wi-Fi address is used instead");
        }
        logMilestones(info);

        // The pipe needs the stream claimed, and only while a phone is linked.
        if (info.peerValid && !stack_.claimed())
            stack_.claim();
        else if (!info.peerValid)
            stack_.release();
        wireless_status::publish(
            session_->phoneConnected()            ? wireless_status::Progress::DetailsSent
            : endpoint_ && endpoint_->handoffSent() ? wireless_status::Progress::WaitingForWifi
            : info.peerValid                      ? wireless_status::Progress::Sending
                                                  : wireless_status::Progress::Waiting);

        serviceBluetooth(info);
        dropBluetoothWhenDone(info, sceKernelGetSystemTimeWide());
        bonjour_.service(sceKernelGetSystemTimeWide());
        network_.poll();
        // The pipe read above already slept when a pipe is up; otherwise this
        // is where the loop waits.
        session_->service(endpoint_ ? SESSION_WAIT_US : IDLE_WAIT_US);
    }
}

} // namespace

app::AppError run(const Config& config, bool (*cancelled)())
{
    Orchestrator orchestrator(config);
    if (!orchestrator.begin(cancelled)) {
        orchestrator.end();
        if (cancelled())
            return app::AppError::None;
        return orchestrator.accessPointFailed() ? app::AppError::WifiApFailed
                                                : app::AppError::CarplayStartFailed;
    }
    orchestrator.run(cancelled);
    orchestrator.end();
    return app::AppError::None;
}

} // namespace carplay
