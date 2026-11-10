#include "transport/phone_network.h"

#include "logger/vita_log.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>

#include <cstdio>

namespace {

constexpr uint64_t AP_POLL_INTERVAL_US = 100000;

bool systemNetwork(WifiNetInfo& info)
{
    info = WifiNetInfo{};
    int state = 0;
    SceNetCtlInfo net{};
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED ||
        sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &net) < 0 || net.ip_address[0] == '\0')
        return false;
    std::snprintf(info.ip, sizeof(info.ip), "%s", net.ip_address);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &net) >= 0)
        std::snprintf(info.ssid, sizeof(info.ssid), "%s", net.ssid);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_BSSID, &net) >= 0)
        info.setBssid(net.bssid.data);
    return true;
}

} // namespace

void PhoneNetwork::begin(const WirelessConfig& config, const char* tag)
{
    end();
    config_ = config;
    tag_ = tag;
    apFailed_ = false;
    WifiNetInfo net;
    if (systemNetwork(net)) {
        vitaLog("%s: on Wi-Fi '%s' at %s; the phone joins that", tag_, net.ssid, net.ip);
        return;
    }
    if (!config_.wifiAp)
        return;
    WifiAp::Config ap;
    ap.ssid = config_.wifiApSsid;
    ap.password = config_.wifiApPassword;
    ap.channel = config_.wifiApChannel;
    apStarted_ = ap_.begin(ap);
    apFailed_ = !apStarted_;
    nextPollUs_ = 0;
    vitaLog("%s: %s the access point '%s' (channel %d)", tag_, apStarted_ ? "starting" : "could not start",
            config_.wifiApSsid, config_.wifiApChannel);
}

void PhoneNetwork::poll()
{
    if (!apStarted_)
        return;
    const uint64_t now = sceKernelGetSystemTimeWide();
    if (now < nextPollUs_)
        return;
    nextPollUs_ = now + AP_POLL_INTERVAL_US;
    const WifiAp::State state = ap_.poll();
    if (state == WifiAp::State::Up && !apUp_) {
        apUp_ = true;
        vitaLog("%s: access point up at %s", tag_, ap_.netInfo().ip);
    } else if (state == WifiAp::State::Failed) {
        vitaLog("%s: the access point failed; carrying on without it", tag_);
        end();
        apFailed_ = true;
    }
}

bool PhoneNetwork::current(Info& info) const
{
    info = Info{};
    if (apUp_) {
        static_cast<WifiNetInfo&>(info) = ap_.netInfo();
        std::snprintf(info.password, sizeof(info.password), "%s", config_.wifiApPassword);
        info.channel = config_.wifiApChannel;
        info.ownAccessPoint = true;
        return true;
    }
    if (!systemNetwork(info))
        return false;
    std::snprintf(info.password, sizeof(info.password), "%s", config_.wifiPassword);
    return true;
}

void PhoneNetwork::end()
{
    if (apStarted_)
        ap_.end();
    apStarted_ = false;
    apUp_ = false;
}
