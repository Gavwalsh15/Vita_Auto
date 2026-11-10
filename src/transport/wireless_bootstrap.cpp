#include "transport/wireless_bootstrap.h"

#include "logger/vita_log.h"

bool WirelessBootstrap::begin(bool (*cancelled)())
{
    apWaiting_ = false;
    apUp_ = false;
    // Bluetooth needs a listener to hand the phone, so both must start.
    if (!listener_.beginOpen(cancelled, false) ||
        !bt_.begin(config_.bluetoothName, config_.recoverDeviceName)) {
        bt_.end();
        listener_.close();
        return false;
    }
    // The access point only helps a phone that Bluetooth can point at it, and
    // only when the Vita has no network of its own to offer.
    if (config_.wifiAp) {
        TcpTransport::WifiNetInfo current;
        if (listener_.queryNetInfo(current)) {
            vitaLog("direct-auto: on Wi-Fi '%s', not starting the Vita's access point",
                    current.ssid);
        } else {
            WifiAp::Config ap;
            ap.ssid = config_.wifiApSsid;
            ap.password = config_.wifiApPassword;
            ap.channel = config_.wifiApChannel;
            apWaiting_ = ap_.begin(ap);
        }
    }
    return true;
}

WirelessBootstrap::Result WirelessBootstrap::poll(bool (*cancelled)())
{
    if (apWaiting_) {
        const WifiAp::State ap = ap_.poll();
        if (ap == WifiAp::State::Up && !apUp_) {
            listener_.setLocalNetwork(&ap_.netInfo());
            apUp_ = true;
        } else if (ap == WifiAp::State::Failed) {
            vitaLog("direct-auto: access point stopped, continuing without it");
            endAccessPoint();
        }
    }

    TcpTransport::WifiNetInfo net;
    listener_.queryNetInfo(net);
    const BtBootstrap::State bt = bt_.poll(
        net, LISTEN_PORT, apUp_ ? config_.wifiApPassword : config_.wifiPassword);
    if (bt == BtBootstrap::State::Failed) {
        vitaLog("direct-auto: Bluetooth stopped, continuing without it");
        end();
        return Result::Stopped;
    }

    const OpenPoll poll = listener_.pollOpen(cancelled);
    if (poll == OpenPoll::Connected)
        return Result::Connected;
    return poll == OpenPoll::Failed ? Result::Failed : Result::Waiting;
}

void WirelessBootstrap::handOff()
{
    bt_.handOff();
}

void WirelessBootstrap::end()
{
    bt_.end();
    endAccessPoint();
    listener_.close();
}

void WirelessBootstrap::endAccessPoint()
{
    ap_.end();
    listener_.setLocalNetwork(nullptr);
    apWaiting_ = false;
    apUp_ = false;
}
