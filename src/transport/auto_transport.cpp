#include "transport/auto_transport.h"

#include <psp2/kernel/threadmgr.h>

#include "logger/vita_log.h"

namespace {
constexpr uint32_t POLL_INTERVAL_US = 100000;
}

AutoTransport::AutoTransport(const Options& options)
    : useUsb_(options.usb),
      useHeadunitServer_(options.headunitServer),
      tcp_(options.wifiHostOverride),
      wireless_(options.wireless)
{
}

bool AutoTransport::closeLosers(Transport* winner)
{
    bool usbStopped = true;
    if (winner != &usb_)
        usbStopped = usb_.close() >= 0;
    if (winner != &tcp_)
        tcp_.close();
    if (winner == &wireless_.transport())
        wireless_.handOff();
    else
        wireless_.end();
    return usbStopped;
}

bool AutoTransport::open(bool (*cancelled)())
{
    clearError();
    active_ = nullptr;
    // The USB wake call is only safe once USB has actually connected.
    Transport::setWakeHandler(nullptr);

    // A side that cannot even start (no USB plugin installed, no networking) is
    // dropped; the others keep waiting. Only losing all of them is an error.
    bool usbWaiting = useUsb_ && usb_.beginOpen(cancelled);
    bool wifiWaiting = useHeadunitServer_ && tcp_.beginOpen(cancelled, false);
    bool btWaiting = wireless_.enabled() && wireless_.begin(cancelled);
    if (useUsb_ && !usbWaiting)
        vitaLog("direct-auto: USB unavailable, continuing without it");
    if (useHeadunitServer_ && !wifiWaiting)
        vitaLog("direct-auto: Wi-Fi Headunit Server unavailable, continuing without it");
    if (wireless_.enabled() && !btWaiting)
        vitaLog("direct-auto: Bluetooth unavailable, continuing without it");
    if (cancelled())
        return false;
    if (!usbWaiting && !wifiWaiting && !btWaiting) {
        // Report the most specific error, USB first.
        if (useUsb_)
            return fail(usb_.lastAppError(), usb_.lastError());
        if (useHeadunitServer_)
            return fail(tcp_.lastAppError(), tcp_.lastError());
        return fail(wireless_.lastAppError(), wireless_.lastError());
    }

    for (;;) {
        if (cancelled())
            return false;

        if (usbWaiting) {
            const OpenPoll poll = usb_.pollOpen(cancelled);
            if (poll == OpenPoll::Connected) {
                vitaLog("direct-auto: USB connected first");
                Transport::setWakeHandler(&UsbTransport::wakeKernel);
                closeLosers(&usb_);
                active_ = &usb_;
                return true;
            }
            if (poll == OpenPoll::Failed) {
                if (cancelled())
                    return false;
                // A phone is plugged in but the handshake failed: report it
                // rather than leave the user waiting on a cable problem.
                fail(usb_.lastAppError(), usb_.lastError());
                return false;
            }
        }

        if (wifiWaiting) {
            const OpenPoll poll = tcp_.pollOpen(cancelled);
            if (poll == OpenPoll::Connected) {
                vitaLog("direct-auto: Wi-Fi connected first");
                if (!closeLosers(&tcp_)) {
                    fail(app::AppError::UsbStopFailed, -1);
                    return false;
                }
                active_ = &tcp_;
                return true;
            }
            if (poll == OpenPoll::Failed) {
                if (cancelled())
                    return false;
                fail(tcp_.lastAppError(), tcp_.lastError());
                return false;
            }
        }

        if (btWaiting) {
            switch (wireless_.poll(cancelled)) {
            case WirelessBootstrap::Result::Connected:
                vitaLog("direct-auto: phone connected after Bluetooth bootstrap");
                if (!closeLosers(&wireless_.transport())) {
                    fail(app::AppError::UsbStopFailed, -1);
                    return false;
                }
                active_ = &wireless_.transport();
                return true;
            case WirelessBootstrap::Result::Failed:
                if (cancelled())
                    return false;
                fail(wireless_.lastAppError(), wireless_.lastError());
                return false;
            case WirelessBootstrap::Result::Stopped:
                btWaiting = false;
                if (!usbWaiting && !wifiWaiting)
                    return fail(app::AppError::NetNotConnected, -3);
                break;
            case WirelessBootstrap::Result::Waiting:
                break;
            }
        }

        sceKernelDelayThread(POLL_INTERVAL_US);
    }
}

int AutoTransport::close()
{
    active_ = nullptr;
    // First, before anything else can end the app: the system panics if it
    // exits with the access point up.
    wireless_.end();
    const int usbResult = usb_.close();
    tcp_.close();
    if (usbResult < 0)
        fail(app::AppError::UsbStopFailed, usbResult);
    return usbResult;
}

bool AutoTransport::send(const uint8_t* data, size_t size)
{
    return active_ != nullptr && active_->send(data, size);
}

int AutoTransport::receive(std::vector<uint8_t>& data, UsbRxTiming* timing)
{
    data.clear();
    if (active_ == nullptr)
        return -1;
    return active_->receive(data, timing);
}

app::AppError AutoTransport::lastAppError() const
{
    if (active_ != nullptr && active_->lastAppError() != app::AppError::None)
        return active_->lastAppError();
    return Transport::lastAppError();
}

int AutoTransport::lastError() const
{
    if (active_ != nullptr && active_->lastAppError() != app::AppError::None)
        return active_->lastError();
    return Transport::lastError();
}
