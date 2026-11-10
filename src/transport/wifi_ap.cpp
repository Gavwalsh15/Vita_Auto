#include "transport/wifi_ap.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>

#include <cstdio>
#include <cstring>

#include "vita_auto_usb.h"
#include "logger/vita_log.h"

namespace {

constexpr const char* CLIENT_BRIDGE_PATH = "app0:vita_auto_usb_client.suprx";
constexpr uint32_t KEEPALIVE_INTERVAL_US = 200000;
constexpr int KEEPALIVE_THREAD_PRIORITY = 0x10000100 - 8;
constexpr int KEEPALIVE_THREAD_STACK = 0x4000;
constexpr uint64_t INFO_LOG_INTERVAL_US = 10000000;

constexpr uint16_t DHCP_SERVER_PORT = 67;
constexpr uint16_t DHCP_CLIENT_PORT = 68;
constexpr uint32_t DHCP_MAGIC = 0x63825363u;
constexpr uint32_t DHCP_FIRST_HOST = 10;
constexpr uint32_t DHCP_LEASE_S = 86400;
constexpr int DHCP_OPTIONS_AT = 240; // 236-byte BOOTP header + magic cookie
constexpr uint8_t DHCP_DISCOVER = 1;
constexpr uint8_t DHCP_OFFER = 2;
constexpr uint8_t DHCP_REQUEST = 3;
constexpr uint8_t DHCP_ACK = 5;
constexpr uint8_t DHCP_NAK = 6;
constexpr int DHCP_RECEIVE_TIMEOUT_US = 100000;

uint32_t getU32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint8_t* putU32(uint8_t* p, uint32_t value)
{
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
    return p + 4;
}

uint8_t* putOption(uint8_t* p, uint8_t code, uint32_t value)
{
    p[0] = code;
    p[1] = 4;
    return putU32(p + 2, value);
}

const char* failureName(int stage)
{
    switch (stage) {
    case VITA_AUTO_AP_FAILURE_NO_MODULE: return "SceWlanBt not loaded";
    case VITA_AUTO_AP_FAILURE_FIRMWARE: return "not the firmware the offsets are for";
    case VITA_AUTO_AP_FAILURE_HOOK: return "hook failed";
    case VITA_AUTO_AP_FAILURE_NO_ADAPTER: return "Wi-Fi driver idle (is Wi-Fi on?)";
    case VITA_AUTO_AP_FAILURE_SYS_CONFIG: return "firmware refused the AP settings";
    case VITA_AUTO_AP_FAILURE_BSS_START: return "firmware did not start the AP";
    case VITA_AUTO_AP_FAILURE_WATCHDOG: return "watchdog (app stopped responding)";
    default: return "unknown";
    }
}

} // namespace

int WifiAp::keepAliveMain(SceSize args, void* argp)
{
    (void)args;
    WifiAp* self = *static_cast<WifiAp**>(argp);
    while (self->keepAliveRun_) {
        vitaAutoApKeepAlive();
        if (self->dhcpSocket_ >= 0)
            self->dhcpPoll(); // Waits up to DHCP_RECEIVE_TIMEOUT_US
        else
            sceKernelDelayThread(KEEPALIVE_INTERVAL_US);
    }
    return sceKernelExitDeleteThread(0);
}

bool WifiAp::begin(const Config& config)
{
    end();
    net_ = WifiNetInfo{};

    const int loadResult = vitaAutoUsbClientLoad(CLIENT_BRIDGE_PATH);
    if (loadResult < 0) {
        vitaLog("wifi-ap: client bridge unavailable (0x%08x)", static_cast<unsigned>(loadResult));
        state_ = State::Failed;
        return false;
    }

    SceNetEtherAddr mac{};
    const int macResult = sceNetGetMacAddress(&mac, 0);
    if (macResult < 0) {
        vitaLog("wifi-ap: no Wi-Fi MAC address (0x%08x)", static_cast<unsigned>(macResult));
        state_ = State::Failed;
        return false;
    }

    VitaAutoApConfig request{};
    std::snprintf(request.ssid, sizeof(request.ssid), "%s", config.ssid);
    std::snprintf(request.passphrase, sizeof(request.passphrase), "%s", config.password);
    // The Wi-Fi address with the locally administered bit set, so the access
    // point does not share an address with the Vita's own interface.
    std::memcpy(request.mac, mac.data, sizeof(request.mac));
    request.mac[0] |= 0x02;
    request.channel = static_cast<uint8_t>(config.channel);

    keepAliveRun_ = true;
    keepAliveThread_ = sceKernelCreateThread("vita-auto-ap-keepalive", keepAliveMain,
                                             KEEPALIVE_THREAD_PRIORITY, KEEPALIVE_THREAD_STACK, 0, 0, nullptr);
    if (keepAliveThread_ < 0) {
        vitaLog("wifi-ap: keep-alive thread failed (0x%08x)",
                static_cast<unsigned>(keepAliveThread_));
        keepAliveThread_ = -1;
        keepAliveRun_ = false;
        state_ = State::Failed;
        return false;
    }
    WifiAp* self = this;
    sceKernelStartThread(keepAliveThread_, sizeof(self), &self);

    const int result = vitaAutoApStart(&request);
    if (result < 0) {
        vitaLog("wifi-ap: start refused (0x%08x)%s", static_cast<unsigned>(result),
                result == -2 ? ": check wifi_ap_ssid, wifi_ap_channel (1-11) and "
                               "wifi_ap_password (empty or 8-63 characters)" : "");
        end();
        state_ = State::Failed;
        return false;
    }

    std::snprintf(net_.ip, sizeof(net_.ip), "%s", VITA_AUTO_AP_IP_TEXT);
    std::snprintf(net_.ssid, sizeof(net_.ssid), "%s", config.ssid);
    net_.setBssid(request.mac);
    vitaLog("wifi-ap: starting '%s' on channel %d (%s), BSSID %s; waiting for the Wi-Fi driver",
            net_.ssid, config.channel, config.password[0] != '\0' ? "WPA2" : "open", net_.bssid);
    state_ = State::Starting;
    nextLogUs_ = 0;
    loggedAssociations_ = 0;
    loggedDhcp_ = 0;
    loggedDhcpOut_ = 0;
    return true;
}

void WifiAp::logInfo(const VitaAutoApInfo& info, bool force)
{
    const uint64_t nowUs = sceKernelGetSystemTimeWide();
    const bool changed = info.associations != loggedAssociations_ ||
                         udpIn_ != loggedDhcp_ || dhcpOut_ != loggedDhcpOut_;
    if (!force && !changed && nowUs < nextLogUs_)
        return;
    nextLogUs_ = nowUs + INFO_LOG_INTERVAL_US;
    loggedAssociations_ = info.associations;
    loggedDhcp_ = udpIn_;
    loggedDhcpOut_ = dhcpOut_;
    const uint8_t* s = info.lastStation;
    vitaLog("wifi-ap: assoc %u deauth %u (last %02x:%02x:%02x:%02x:%02x:%02x, event 0x%04x); "
            "port 67 in %u, DHCP in %u out %u (last send 0x%08x, offered .%u); "
            "driver cmds %u, fw msgs %u",
            info.associations, info.deauths, s[0], s[1], s[2], s[3], s[4], s[5],
            info.lastEvent, udpIn_, dhcpIn_, dhcpOut_,
            static_cast<unsigned>(dhcpLastSend_), dhcpLastOffer_ & 0xff,
            info.commandsSeen, info.responses);

    vitaLog("wifi-ap: tx kicks %u frames %u stamped %u; driver flags 0x%08x",
            info.txKicks, info.txFrames, info.txStamped, info.wlanFlags);
}

WifiAp::State WifiAp::poll()
{
    if (state_ != State::Starting && state_ != State::Up)
        return state_;
    VitaAutoApInfo info{};
    if (vitaAutoApGetInfo(&info) < 0)
        return state_;

    if (info.state == VITA_AUTO_AP_FAILED) {
        vitaLog("wifi-ap: failed: %s (stage %d, result 0x%08x, SceWlanBt nid 0x%08x)",
                failureName(info.failureStage), info.failureStage,
                static_cast<unsigned>(info.failureResult), info.moduleNid);

        vitaLog("wifi-ap: command failure %d, driver pool 0x%02x, in flight 0x%08x; "
                "previous stop %d (still in flight 0x%08x)",
                info.commandFailure, info.poolMask, info.pendingAtFail,
                info.stopResult, info.stopPending);
        end();
        state_ = State::Failed;
        return state_;
    }
    if (state_ == State::Starting && info.state == VITA_AUTO_AP_UP) {
        const int ifResult = vitaAutoApConfigureInterface();
        dhcpOpen();
        vitaLog("wifi-ap: up after %u ms driver wait (config variant %d, SceWlanBt nid 0x%08x); "
                "wlan0 %s flags 0x%08x; DHCP socket 0x%08x",
                info.adapterWaitMs, info.configVariant, info.moduleNid,
                VITA_AUTO_AP_IP_TEXT, static_cast<unsigned>(ifResult),
                static_cast<unsigned>(dhcpSocketResult_));
        state_ = State::Up;
        logInfo(info, true);
    } else if (state_ == State::Up) {
        logInfo(info, false);
    }
    return state_;
}

void WifiAp::end()
{
    if (keepAliveThread_ >= 0) {
        keepAliveRun_ = false;
        sceKernelWaitThreadEnd(keepAliveThread_, nullptr, nullptr);
        keepAliveThread_ = -1;
    }
    dhcpClose();
    if (state_ == State::Starting || state_ == State::Up) {
        vitaAutoApKeepAlive();
        VitaAutoApInfo info{};
        if (vitaAutoApGetInfo(&info) >= 0)
            logInfo(info, true);
        const int result = vitaAutoApStop();
        VitaAutoApInfo after{};
        if (vitaAutoApGetInfo(&after) >= 0)
            vitaLog("wifi-ap: stopped (0x%08x); BSS_STOP result %d, command still in flight 0x%08x",
                    static_cast<unsigned>(result), after.stopResult, after.stopPending);
        else
            vitaLog("wifi-ap: stopped (0x%08x)", static_cast<unsigned>(result));
    }
    if (state_ != State::Failed)
        state_ = State::Off;
}

void WifiAp::dhcpOpen()
{
    dhcpClose();
    udpIn_ = dhcpIn_ = dhcpOut_ = 0;
    dhcpLastSend_ = 0;
    dhcpLastOffer_ = 0;
    dhcpLeaseCount_ = 0;
    dhcpNextVictim_ = 0;

    const int sock = sceNetSocket("vita_auto_dhcp", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    dhcpSocketResult_ = sock;
    if (sock < 0)
        return;
    const int one = 1;
    const int timeoutUs = DHCP_RECEIVE_TIMEOUT_US;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &one, sizeof(one));
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeoutUs, sizeof(timeoutUs));
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(DHCP_SERVER_PORT);
    const int bound = sceNetBind(sock, reinterpret_cast<const SceNetSockaddr*>(&addr), sizeof(addr));
    if (bound < 0) {
        sceNetSocketClose(sock);
        dhcpSocketResult_ = bound;
        return;
    }
    dhcpSocket_ = sock;
}

void WifiAp::dhcpClose()
{
    if (dhcpSocket_ >= 0) {
        sceNetSocketClose(dhcpSocket_);
        dhcpSocket_ = -1;
    }
}

uint32_t WifiAp::dhcpLease(const uint8_t* chaddr)
{
    uint32_t i = 0;
    while (i < dhcpLeaseCount_ && std::memcmp(dhcpLeases_[i], chaddr, 6) != 0)
        ++i;
    if (i == dhcpLeaseCount_) {
        // Android picks a new random MAC for each association
        constexpr uint32_t SLOTS = sizeof(dhcpLeases_) / sizeof(dhcpLeases_[0]);
        if (dhcpLeaseCount_ == SLOTS) {
            i = dhcpNextVictim_;
            dhcpNextVictim_ = (dhcpNextVictim_ + 1) % SLOTS;
        } else {
            ++dhcpLeaseCount_;
        }
        std::memcpy(dhcpLeases_[i], chaddr, 6);
    }
    return (VITA_AUTO_AP_IP & VITA_AUTO_AP_NETMASK) | (DHCP_FIRST_HOST + i);
}

void WifiAp::dhcpPoll()
{
    uint8_t* pkt = dhcpPacket_;
    const int length = sceNetRecvfrom(dhcpSocket_, pkt, sizeof(dhcpPacket_), 0, nullptr, nullptr);
    if (length <= 0)
        return;
    ++udpIn_;
    if (length < DHCP_OPTIONS_AT || pkt[0] != 1 || pkt[1] != 1 || pkt[2] != 6 ||
        getU32(pkt + 236) != DHCP_MAGIC)
        return;

    uint8_t type = 0;
    uint32_t requested = getU32(pkt + 12); // ciaddr, unless option 50 names one
    uint32_t serverId = 0;
    for (int at = DHCP_OPTIONS_AT; at + 1 < length && pkt[at] != 255;) {
        const uint8_t code = pkt[at];
        if (code == 0) {
            ++at;
            continue;
        }
        const uint8_t size = pkt[at + 1];
        if (at + 2 + size > length)
            break;
        if (code == 53 && size == 1)
            type = pkt[at + 2];
        else if (code == 50 && size == 4)
            requested = getU32(pkt + at + 2);
        else if (code == 54 && size == 4)
            serverId = getU32(pkt + at + 2);
        at += 2 + size;
    }
    if (type != DHCP_DISCOVER && type != DHCP_REQUEST)
        return;
    ++dhcpIn_;
    // A REQUEST for another server's offer is not ours to answer.
    if (type == DHCP_REQUEST && serverId != 0 && serverId != VITA_AUTO_AP_IP)
        return;
    const uint32_t offer = dhcpLease(pkt + 28);
    const uint8_t reply = type == DHCP_DISCOVER ? DHCP_OFFER
        : requested == offer ? DHCP_ACK : DHCP_NAK;

    // Reuse the request: same xid, flags, giaddr and chaddr.
    pkt[0] = 2; // BOOTREPLY
    pkt[3] = 0; // hops
    std::memset(pkt + 8, 0, 2);   // secs
    std::memset(pkt + 12, 0, 4);  // ciaddr
    putU32(pkt + 16, reply == DHCP_NAK ? 0 : offer); // yiaddr
    std::memset(pkt + 20, 0, 4);  // siaddr
    std::memset(pkt + 44, 0, 192); // sname, file
    uint8_t* p = pkt + DHCP_OPTIONS_AT;
    *p++ = 53;
    *p++ = 1;
    *p++ = reply;
    p = putOption(p, 54, VITA_AUTO_AP_IP);
    if (reply != DHCP_NAK) {
        // Android only counts a network as up with a route and DNS.
        p = putOption(p, 51, DHCP_LEASE_S);
        p = putOption(p, 1, VITA_AUTO_AP_NETMASK);
        p = putOption(p, 3, VITA_AUTO_AP_IP);
        p = putOption(p, 6, VITA_AUTO_AP_IP);
    }
    *p++ = 255;
    int replyLength = static_cast<int>(p - pkt);
    if (replyLength < 300) {
        std::memset(p, 0, 300 - replyLength);
        replyLength = 300;
    }

    SceNetSockaddrIn to{};
    to.sin_len = sizeof(to);
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(DHCP_CLIENT_PORT);
    to.sin_addr.s_addr = 0xffffffffu;
    int sent = sceNetSendto(dhcpSocket_, pkt, replyLength, 0,
                            reinterpret_cast<const SceNetSockaddr*>(&to), sizeof(to));
    if (sent < 0) {
        to.sin_addr.s_addr = sceNetHtonl(VITA_AUTO_AP_IP | ~VITA_AUTO_AP_NETMASK);
        sent = sceNetSendto(dhcpSocket_, pkt, replyLength, 0,
                            reinterpret_cast<const SceNetSockaddr*>(&to), sizeof(to));
    }
    dhcpLastSend_ = sent;
    if (sent >= 0) {
        ++dhcpOut_;
        dhcpLastOffer_ = offer;
    }
}
