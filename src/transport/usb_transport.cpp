#include "transport/usb_transport.h"

#include <psp2/kernel/threadmgr.h>

#include "vita_auto_usb.h"
#include "logger/vita_log.h"

namespace {

constexpr const char* CLIENT_BRIDGE_PATH = "app0:vita_auto_usb_client.suprx";
constexpr uint32_t POLL_INTERVAL_US = 100000;
constexpr uint64_t OPEN_TIMEOUT_US = 60000000;
constexpr uint32_t WRITE_RETRY_INTERVAL_US = 250;
constexpr uint32_t WRITE_TIMEOUT_US = 250000;
constexpr uint32_t STOP_RETRY_DELAY_US = 50000;

}

bool UsbTransport::fail(app::AppError error, int detail)
{
    if (lastAppError_ == app::AppError::None) {
        lastAppError_ = error;
        lastError_ = detail;
    }
    vitaLogAppError(error, detail);
    return false;
}

bool UsbTransport::open(bool (*cancelled)())
{
    lastAppError_ = app::AppError::None;
    lastError_ = 0;
    if (cancelled())
        return false;

    vitaLogInfo("direct-usb: loading USB client bridge");
    const int loadResult = vitaAutoUsbClientLoad(CLIENT_BRIDGE_PATH);
    vitaLogInfo("direct-usb: client load result=0x%08x", loadResult);
    if (loadResult < 0) {
        vitaLog("direct-usb: client bridge unavailable; check vita_auto_usb.skprx and reboot");
        return fail(app::AppError::UsbBridgeLoadFailed, loadResult);
    }

    // Even a failed Start may hold onto resources if its rollback could not finish.
    sessionStarted_ = true;
    const int startResult = vitaAutoUsbStart();
    const int currentHostMode = vitaAutoUsbGetCurrentHostMode();
    vitaLogInfo(
        "direct-usb: app host start=0x%08x currentMode=%d",
        startResult,
        currentHostMode
    );
    if (startResult < 0 || currentHostMode != 0) {
        fail(app::AppError::UsbHostStartFailed, startResult < 0 ? startResult : currentHostMode);
        vitaLog("direct-usb: Vita port 2 did not enter host mode");
        return false;
    }

    vitaLogInfo(
        "direct-usb: kernel accessory=0x%08x hostMode=0x%08x driver=0x%08x vidpid=0x%08x failure=%d/0x%08x/%d",
        vitaAutoUsbGetAccessoryActivateResult(),
        vitaAutoUsbGetHostModeResult(),
        vitaAutoUsbGetDriverRegisterResult(),
        vitaAutoUsbGetLastVidPid(),
        vitaAutoUsbGetLastFailureStage(),
        vitaAutoUsbGetLastFailureResult(),
        vitaAutoUsbGetLastFailureCount()
    );

    uint64_t connectionDeadlineUs = 0;
    int lastState = -1;
    unsigned int lastVidPid = 0xffffffffu;

    for (;;)
    {
        if (cancelled()) {
            if (VITA_AUTO_VERBOSE_LOGGING && connectionDeadlineUs != 0)
                logRxDiagnostics("open-cancelled");
            return false; // The owner still closes the session before exiting.
        }
        const uint64_t nowUs = sceKernelGetSystemTimeWide();
        const int state = vitaAutoUsbGetState();
        const unsigned int vidPid = vitaAutoUsbGetLastVidPid();

        // Waiting for a phone is unlimited. Once enumeration starts, retries
        // within the handshake must not keep extending this attempt.
        if (connectionDeadlineUs == 0 && state != VITA_AUTO_USB_IDLE)
            connectionDeadlineUs = nowUs + OPEN_TIMEOUT_US;

        if (state != lastState || vidPid != lastVidPid) {
            vitaLogInfo(
                "direct-usb: state=%d vidpid=0x%08x failure=%d/0x%08x/%d",
                state,
                vidPid,
                vitaAutoUsbGetLastFailureStage(),
                vitaAutoUsbGetLastFailureResult(),
                vitaAutoUsbGetLastFailureCount()
            );
            if (VITA_AUTO_VERBOSE_LOGGING && state != VITA_AUTO_USB_IDLE &&
                state < lastState)
                logRxDiagnostics("handshake-retry");
            lastState = state;
            lastVidPid = vidPid;
        }

        if (state == VITA_AUTO_USB_AA_VERSION_COMPLETE)
        {
            connected_ = true;
            return true;
        }

        if (state == VITA_AUTO_USB_FAILED) {
            const bool stale = vitaAutoUsbGetLastFailureStage() ==
                VITA_AUTO_USB_FAILURE_STALE_ACCESSORY;
            fail(
                stale ? app::AppError::UsbStaleAccessory
                      : app::AppError::UsbEnumerationFailed,
                vitaAutoUsbGetLastFailureResult()
            );
            logRxDiagnostics("open-failed");
            return false;
        }

        if (connectionDeadlineUs != 0 && nowUs >= connectionDeadlineUs)
            break;

        sceKernelDelayThread(POLL_INTERVAL_US);
    }

    vitaLog(
        "direct-usb: timeout state=%d vidpid=0x%08x; no phone enumerated if vidpid is zero",
        vitaAutoUsbGetState(),
        vitaAutoUsbGetLastVidPid()
    );

    logRxDiagnostics("open-timeout");
    return fail(app::AppError::UsbConnectTimedOut);
}

int UsbTransport::close()
{
    connected_ = false;
    if (!sessionStarted_)
        return 0;
    vitaLogInfo("direct-usb: stopping USB session");
    int result = vitaAutoUsbStop();
    if (result < 0) {
        // Stop retains the session on failure; a second call can finish
        // draining callbacks that completed after the first timeout.
        sceKernelDelayThread(STOP_RETRY_DELAY_US);
        result = vitaAutoUsbStop();
    }
    if (result < 0) {
        fail(app::AppError::UsbStopFailed, result);
        return result;
    }
    sessionStarted_ = false;
    vitaLogInfo("direct-usb: USB session stopped");
    return result;
}

bool UsbTransport::send(const uint8_t* data, size_t size)
{
    if (!connected_ || data == nullptr || size == 0 || size > MAX_TRANSFER_SIZE)
        return fail(app::AppError::UsbSendRejected);

    uint32_t waitedUs = 0;
    size_t offset = 0;
    while (waitedUs <= WRITE_TIMEOUT_US) {
        size_t count = size - offset;
        // USB 2 bulk packet sizes are 8/16/32/64/512 bytes. Splitting a
        // multiple of 8 guarantees a short packet without changing the stream.
        if (count % 8 == 0)
            --count;
        const int written = vitaAutoUsbWrite(
            data + offset,
            static_cast<uint32_t>(count)
        );
        if (written < 0 || (written > 0 && written != static_cast<int>(count))) {
            connected_ = false;
            return fail(app::AppError::UsbWriteFailed, written);
        }
        if (written > 0) {
            offset += static_cast<size_t>(written);
            if (offset == size)
                return true;
            continue;
        }

        sceKernelDelayThread(WRITE_RETRY_INTERVAL_US);
        waitedUs += WRITE_RETRY_INTERVAL_US;
    }

    connected_ = false;
    return fail(app::AppError::UsbWriteTimedOut);
}

int UsbTransport::receive(
    std::vector<uint8_t>& data,
    UsbRxTiming* timing
)
{
    data.clear();
    if (timing != nullptr)
        *timing = UsbRxTiming{};

    if (!connected_) {
        fail(app::AppError::UsbReadFailed);
        return -1;
    }

    uint8_t buffer[MAX_TRANSFER_SIZE];
    VitaAutoUsbReadMetadata metadata{};
    const int received = vitaAutoUsbReadWaitTimed(
        buffer,
        sizeof(buffer),
        10000,
        &metadata
    );

    if (received < 0)
    {
        connected_ = false;
        fail(app::AppError::UsbReadFailed, received);
        return -1;
    }

    if (received == 0)
        return 0;

    if (timing != nullptr) {
        timing->firstCompletedAtUs = metadata.firstCompletedAtUs;
        timing->lastCompletedAtUs = metadata.lastCompletedAtUs;
    }
    data.assign(buffer, buffer + received);
    return 1;
}

void UsbTransport::wake()
{
    vitaAutoUsbWake();
}

void UsbTransport::logRxDiagnostics(const char* reason) const
{
    VitaAutoUsbRxDiagnostics diagnostics{};
    const int result = vitaAutoUsbGetRxDiagnostics(&diagnostics);
    if (result < 0) {
        vitaLog(
            "direct-usb: RX diagnostics unavailable reason=%s err=0x%08x",
            reason != nullptr ? reason : "unknown",
            result
        );
        return;
    }

    vitaLog(
        "direct-usb: IO diag reason=%s rx_submit=%u rx_complete=%u publish=%u consume=%u depth=%u peak=%u overflow=%u checksum=%u sequence=%u armed=%u tx_submit=%u tx_complete=%u tx_fail=%u tx_busy=%u tx_last=0x%08x/%d",
        reason != nullptr ? reason : "unknown",
        diagnostics.submittedTransfers,
        diagnostics.completedTransfers,
        diagnostics.publishedPackets,
        diagnostics.consumedPackets,
        diagnostics.ringDepth,
        diagnostics.maxRingDepth,
        diagnostics.overflowCount,
        diagnostics.checksumFailures,
        diagnostics.sequenceFailures,
        diagnostics.readArmed,
        diagnostics.submittedWrites,
        diagnostics.completedWrites,
        diagnostics.writeFailures,
        diagnostics.writeBusy,
        diagnostics.lastWriteResult,
        diagnostics.lastWriteBytes
    );
}

bool UsbTransport::connected() const
{
    return connected_;
}
