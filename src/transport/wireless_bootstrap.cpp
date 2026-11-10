#include "transport/wireless_bootstrap.h"

#include "logger/vita_log.h"

bool WirelessBootstrap::begin(bool (*cancelled)())
{
    apUp_ = false;
    // Bluetooth needs a listener to hand the phone, so both must start.
    if (!listener_.beginOpen(cancelled) ||
        !bt_.begin(config_.bluetoothName, config_.recoverDeviceName)) {
        bt_.end();
        listener_.close();
        return false;
    }
    network_.begin(config_, "direct-auto");
    return true;
}

WirelessBootstrap::Result WirelessBootstrap::poll(bool (*cancelled)())
{
    network_.poll();
    if (network_.accessPointUp() != apUp_) {
        apUp_ = network_.accessPointUp();
        listener_.useOwnAccessPoint(apUp_);
    }

    PhoneNetwork::Info net;
    network_.current(net);
    const BtBootstrap::State bt = bt_.poll(net, LISTEN_PORT, net.password);
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
    listener_.close();
    network_.end();
    listener_.useOwnAccessPoint(false);
    apUp_ = false;
    bt_.end();
}
