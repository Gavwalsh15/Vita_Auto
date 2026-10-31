#include "app/direct_session.h"
#include "audio/audio_sink.h"
#include "app/config.h"
#include "inputs/vita_input.h"
#include "logger/performance.h"
#include "video/video_pipeline.h"
#include "android_auto/aa_direct.h"
#include "audio/audio_output.h"
#include "audio/microphone_input.h"
#include "logger/vita_log.h"
#include "vita_auto_usb.h"
#include "transport/auto_transport.h"
#include "transport/tcp_transport.h"
#include "transport/usb_transport.h"

#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <cstdio>
namespace app {
namespace direct_session {
namespace performance = logger::performance;

namespace {

// Only this thread touches transport and TLS state. Input arrives via its queue.
constexpr uint64_t ANDROID_AUTO_START_TIMEOUT_US = 60 * 1000 * 1000;
constexpr uint64_t SHUTDOWN_DRAIN_TIMEOUT_US = 500000;
constexpr uint64_t SHUTDOWN_PING_INTERVAL_US = 50000;
// Match the user-thread priority used by the decoder/audio workers.
constexpr int DIRECT_USB_THREAD_PRIORITY = 0x10000100;
constexpr int DIRECT_USB_THREAD_STACK_SIZE = 0x40000;
static SceUID g_directUsbThread = -1;
// Set before the IO thread starts and kept fixed until it has joined.
static bool g_audioOnVita = true;
static ConnectionMode g_mode = ConnectionMode::Auto;
static char g_host[40] = {};
static volatile uint32_t g_directIoFinished = 0;
static volatile int g_directIoError = static_cast<int>(AppError::None);
static volatile int g_directIoErrorDetail = 0;
static volatile uint32_t g_stopRequested = 0;
static volatile uint32_t g_canRestart = 1;
static volatile int g_status = static_cast<int>(Status::WaitingForPhone);

bool stopRequested()
{
    return __atomic_load_n(&g_stopRequested, __ATOMIC_ACQUIRE) != 0;
}

void recordAppError(AppError error, int detail = 0)
{
    if (error == AppError::None)
        return;
    int expected = static_cast<int>(AppError::None);
    if (__atomic_compare_exchange_n(&g_directIoError, &expected,
            static_cast<int>(error), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_directIoErrorDetail, detail, __ATOMIC_RELEASE);
        vitaLogAppError(error, detail);
    }
}

void watchUsbAfterFailure(uint32_t durationMs)
{
    vitaLog(
        "direct-usb: post-failure watch start duration_ms=%u",
        durationMs
    );
    int lastState = -1;
    unsigned int lastVidPid = 0xffffffffu;
    uint64_t lastFailure = ~static_cast<uint64_t>(0);
    int lastHostMode = -1;
    const uint64_t startedAtUs = sceKernelGetSystemTimeWide();
    while (sceKernelGetSystemTimeWide() - startedAtUs <
           static_cast<uint64_t>(durationMs) * 1000)
    {
        const int state = vitaAutoUsbGetState();
        const unsigned int vidPid = vitaAutoUsbGetLastVidPid();
        const int stage = vitaAutoUsbGetLastFailureStage();
        const int result = vitaAutoUsbGetLastFailureResult();
        const int count = vitaAutoUsbGetLastFailureCount();
        const int hostMode = vitaAutoUsbGetCurrentHostMode();
        VitaAutoUsbRxDiagnostics diagnostics{};
        const int diagResult = vitaAutoUsbGetRxDiagnostics(&diagnostics);
        const uint64_t failureKey =
            (static_cast<uint64_t>(static_cast<uint32_t>(stage)) << 32) |
            static_cast<uint32_t>(result);
        if (state != lastState || vidPid != lastVidPid ||
            failureKey != lastFailure || hostMode != lastHostMode)
        {
            vitaLog(
                "direct-usb: post-failure state=%d vidpid=0x%08x hostmode=%d failure=%d/0x%08x/%d armed=%u",
                state,
                vidPid,
                hostMode,
                stage,
                result,
                count,
                diagResult >= 0 ? diagnostics.readArmed : 0
            );
            lastState = state;
            lastVidPid = vidPid;
            lastFailure = failureKey;
            lastHostMode = hostMode;
        }
        sceKernelDelayThread(100000);
    }
    vitaLog(
        "direct-usb: post-failure watch end state=%d vidpid=0x%08x hostmode=%d",
        vitaAutoUsbGetState(),
        vitaAutoUsbGetLastVidPid(),
        vitaAutoUsbGetCurrentHostMode()
    );
}

bool closeTransport(Transport& transport)
{
    const int stopResult = transport.close();
    const bool stopped = stopResult >= 0;
    __atomic_store_n(&g_canRestart, stopped ? 1u : 0u, __ATOMIC_RELEASE);
    if (!stopped)
        recordAppError(AppError::UsbStopFailed, stopResult);
    return stopped;
}

bool flushDirectSessionOutbound(
    Transport& transport,
    android_auto::DirectSession& session
)
{
    const uint8_t* wire = session.outboundData();
    const size_t wireSize = session.outboundSize();
    if (wireSize == 0)
        return true;

    const uint64_t writeStartUs = performance::nowUs();
    performance::add(performance::Count::UsbTxBytes, static_cast<uint32_t>(wireSize));

    size_t offset = 0;
    while (offset < wireSize) {
        const size_t count = std::min(
            Transport::MAX_TRANSFER_SIZE,
            wireSize - offset
        );
        if (!transport.send(wire + offset, count)) {
            performance::record(performance::Metric::UsbTx, performance::elapsedUs(writeStartUs));
            return false;
        }
        offset += count;
    }

    if (VITA_AUTO_PERFORMANCE)
        session.markOutboundSubmitted(sceKernelGetSystemTimeWide());
    session.clearOutbound();
    performance::record(performance::Metric::UsbTx, performance::elapsedUs(writeStartUs));
    return true;
}

bool drainReadsBeforeShutdown(
    Transport& transport,
    android_auto::DirectSession& session
)
{
    const int result = transport.beginShutdownDrain();
    if (result < 0) {
        vitaLog("direct-usb: could not begin RX drain err=0x%08x", result);
        return false;
    }

    const uint64_t deadlineUs =
        sceKernelGetSystemTimeWide() + SHUTDOWN_DRAIN_TIMEOUT_US;
    uint64_t nextPingUs = 0;
    std::vector<uint8_t> discarded;
    TransportDrainState diagnostics{};
    while (sceKernelGetSystemTimeWide() < deadlineUs) {
        if (transport.shutdownDrainState(diagnostics) < 0)
            break;
        if (diagnostics.readsArmed == 0) {
            vitaLogInfo("direct-usb: RX drained before shutdown");
            return true;
        }

        const uint64_t nowUs = sceKernelGetSystemTimeWide();
        if (!diagnostics.writeBusy && nowUs >= nextPingUs) {
            if (!session.sendPingRequest() ||
                !flushDirectSessionOutbound(transport, session))
                break;
            nextPingUs = nowUs + SHUTDOWN_PING_INTERVAL_US;
        }
        // The session is ending; consume final packets without decoding media
        // or creating more acknowledgements. Pings also drain an idle screen.
        if (transport.receive(discarded) < 0)
            break;
    }
    vitaLog("direct-usb: RX drain incomplete armed=%u; Stop will retain pending requests",
            diagnostics.readsArmed);
    return false;
}

int runDirectSession()
{
    std::unique_ptr<Transport> transportOwner;
    switch (g_mode) {
    case ConnectionMode::Wireless:
    case ConnectionMode::Auto: {
        AutoTransport::Options options;
        options.wifiHostOverride = g_host;
        options.usb = g_mode == ConnectionMode::Auto;
        transportOwner.reset(new AutoTransport(options));
        break;
    }
    case ConnectionMode::Usb:
        transportOwner.reset(new UsbTransport());
        break;
    }
    Transport& transport = *transportOwner;
    vitaLog("session: connection=%s", connectionModeName(g_mode));
    android_auto::DirectSession session(g_audioOnVita);
    audio::MicrophoneInput microphoneInput(&Transport::wake);
    audio::sink::AudioSinkStats audioStats{};
    uint32_t videoSequence = 0;
    uint32_t videoFrames = 0;
    performance::Reporter perfReporter{};
    std::vector<uint8_t> packet;
    packet.reserve(Transport::MAX_TRANSFER_SIZE);

    if (!transport.open(&stopRequested)) {
        recordAppError(transport.lastAppError(), transport.lastError());
        if (!stopRequested())
            vitaLog("DIRECT USB open failed");
        closeTransport(transport);
        return lastError() == AppError::None ? 0 : 1;
    }
    vitaLogInfo("DIRECT USB open ok");
    __atomic_store_n(&g_status, static_cast<int>(Status::Connecting), __ATOMIC_RELEASE);
    if (stopRequested()) {
        closeTransport(transport);
        return lastError() == AppError::None ? 0 : 1;
    }

    // USB keeps the proven 80 ms queue. Wi-Fi uses a deeper buffer to ride out
    // 100-200 ms stalls; music and navigation prompts are not latency sensitive.
    const uint32_t mediaBufferMs = transport.wireless() ? 400 : 80;
    const uint32_t promptBufferMs = transport.wireless() ? 250 : 80;
    audio::Output mediaAudioOutput(48000, 2, mediaBufferMs);
    audio::Output promptAudioOutput(16000, 1, promptBufferMs);

    session.setMediaMaxUnacked(transport.mediaMaxUnacked());
    session.setMediaHandler(
        [&](const android_auto::MediaFrame& frame) {
            if (audio::sink::acceptFrame(
                    frame,
                    mediaAudioOutput,
                    promptAudioOutput,
                    audioStats))
            {
                return;
            }
            video::pipeline::acceptFrame(
                frame,
                videoSequence,
                videoFrames,
                "direct-usb"
            );
        }
    );

    const bool sessionStarted = transport.versionNegotiated()
        ? session.startAfterVersion() : session.start();
    if (!sessionStarted ||
        !flushDirectSessionOutbound(transport, session))
    {
        recordAppError(sessionStarted ? transport.lastAppError() : AppError::AndroidAutoStartFailed,
                       sessionStarted ? transport.lastError() : session.lastError());
        vitaLog("direct-usb: session startup failed app=%s aa=%d (%s)",
                lastErrorName(), session.lastError(), session.lastErrorName());
        closeTransport(transport);
        inputs::setReady(false);
        return 1;
    }

    android_auto::DirectState lastState = session.state();
    vitaLogInfo("direct-usb: session state=%d", static_cast<int>(lastState));

    auto syncMicrophoneCapture = [&]() {
        if (!microphoneInput.setEnabled(session.microphoneOpen())) {
            recordAppError(microphoneInput.lastAppError(), microphoneInput.lastError());
            vitaLog(
                "direct-usb: microphone worker failed err=0x%08x (%s)",
                microphoneInput.lastError(), microphoneInput.lastErrorName()
            );
            return false;
        }
        return true;
    };

    auto drainMicrophoneFrames = [&]() {
        audio::MicrophoneInput::Frame frame;
        while (session.microphoneCanSend() && microphoneInput.pop(frame)) {
            if (!session.sendMicrophone(
                    frame.samples.data(),
                    frame.samples.size(),
                    frame.capturedAtUs))
            {
                recordAppError(AppError::MicrophoneSendFailed, session.lastError());
                return false;
            }
        }
        return true;
    };

    const uint64_t sessionStartedAtUs = sceKernelGetSystemTimeWide();
    while (!stopRequested() && transport.connected() &&
           session.state() != android_auto::DirectState::Error)
    {
        const video::pipeline::Failure videoFailure = video::pipeline::failure();
        if (videoFailure.error != AppError::None) {
            recordAppError(videoFailure.error, videoFailure.detail);
            break;
        }
        if (!video::pipeline::hasPresentedFrame() &&
            sceKernelGetSystemTimeWide() - sessionStartedAtUs >=
                ANDROID_AUTO_START_TIMEOUT_US)
        {
            recordAppError(AppError::AndroidAutoStartTimedOut);
            break;
        }
        const uint32_t inputDeliveredBefore = inputs::deliveryStats().delivered;
        inputs::setReady(session.inputReady());

        if (!syncMicrophoneCapture() || !drainMicrophoneFrames()) {
            vitaLog("direct-usb: microphone capture/send failed");
            break;
        }

        if (!inputs::drain(session)) {
            recordAppError(AppError::InputSendFailed, session.lastError());
            vitaLog("direct-usb: queued input rejected");
            break;
        }

        if (!flushDirectSessionOutbound(transport, session)) {
            recordAppError(transport.lastAppError(), transport.lastError());
            vitaLog("direct-usb: input write failed");
            break;
        }
        if (inputs::deliveryStats().delivered != inputDeliveredBefore)
            video::pipeline::noteInputSubmitted(performance::nowUs());

        const uint64_t receiveStartUs = performance::nowUs();
        UsbRxTiming rxTiming{};
        const int received = transport.receive(packet, &rxTiming);
        const uint64_t receiveReturnedUs = performance::nowUs();
        performance::record(performance::Metric::UsbRx, performance::elapsedUs(receiveStartUs));
        if (received > 0) {
            performance::add(performance::Count::UsbRxBytes, static_cast<uint32_t>(packet.size()));
            if (rxTiming.firstCompletedAtUs != 0 &&
                receiveReturnedUs >= rxTiming.firstCompletedAtUs)
            {
                performance::record(performance::Metric::KernelFirstAge,
                    static_cast<uint32_t>(
                        receiveReturnedUs - rxTiming.firstCompletedAtUs
                    )
                );
            }
            if (rxTiming.lastCompletedAtUs != 0 &&
                receiveReturnedUs >= rxTiming.lastCompletedAtUs)
            {
                performance::record(performance::Metric::KernelLastAge,
                    static_cast<uint32_t>(
                        receiveReturnedUs - rxTiming.lastCompletedAtUs
                    )
                );
            }
            if (rxTiming.firstCompletedAtUs != 0 &&
                rxTiming.lastCompletedAtUs >= rxTiming.firstCompletedAtUs)
            {
                performance::record(performance::Metric::KernelBatchSpan,
                    static_cast<uint32_t>(
                        rxTiming.lastCompletedAtUs -
                        rxTiming.firstCompletedAtUs
                    )
                );
            }
        }

        if (received < 0) {
            recordAppError(transport.lastAppError(), transport.lastError());
            break;
        }

        if (stopRequested())
            break;

        bool feedOk = true;
        if (received > 0) {
            const uint64_t feedStartUs = performance::nowUs();
            feedOk = session.feed(
                packet.data(),
                packet.size(),
                rxTiming.firstCompletedAtUs,
                rxTiming.lastCompletedAtUs
            );
            performance::record(performance::Metric::SessionFeed, performance::elapsedUs(feedStartUs));
        }

        if (!feedOk)
        {
            recordAppError(AppError::AndroidAutoFeedFailed, session.lastError());
            vitaLog(
                "direct-usb: session feed failed err=%d (%s) tls=0x%08x",
                session.lastError(),
                session.lastErrorName(),
                session.lastTlsError()
            );
            transport.logDiagnostics("session-feed-failed");
            break;
        }

        const android_auto::DirectState state = session.state();
        if (state != lastState) {
            vitaLogInfo("direct-usb: session state=%d", static_cast<int>(state));
            lastState = state;
        }

        transport.logHeartbeat(videoFrames);

        // A microphone request is delivered by session.feed(). Start/stop the
        // blocking capture worker before sleeping for more USB input.
        if (!syncMicrophoneCapture() || !drainMicrophoneFrames()) {
            vitaLog("direct-usb: microphone capture/send failed");
            break;
        }

        /* Flush media ACKs and protocol responses produced by session.feed
         Touch was already sent at the start of this iteration. */
        if (!flushDirectSessionOutbound(transport, session)) {
            recordAppError(transport.lastAppError(), transport.lastError());
            vitaLog("direct-usb: session write failed");
            break;
        }

        if (VITA_AUTO_PERFORMANCE &&
            perfReporter.report(videoFrames, session))
            transport.logDiagnostics("perf-ack");

        // receive() sleeps on the kernel RX/input event. No user-space poll
        // delay is needed here.
    }

    inputs::setReady(false);
    const AppError transportError = transport.lastAppError();
    if (!stopRequested() &&
        (transportError == AppError::UsbReadFailed ||
         transportError == AppError::UsbWriteFailed))
    {
        watchUsbAfterFailure(10000);
    }
    if (stopRequested() && transport.connected()) {
        const bool drained = drainReadsBeforeShutdown(transport, session);
        if (!drained && transport.lastAppError() != AppError::None)
            recordAppError(transport.lastAppError(), transport.lastError());
        // An unresponsive peer may still own a pending write after the drain.
        if (drained && transport.connected() && session.sendShutdownRequest() &&
            !flushDirectSessionOutbound(transport, session))
            recordAppError(transport.lastAppError(), transport.lastError());
    }

    if (session.state() == android_auto::DirectState::Error && !stopRequested())
        recordAppError(AppError::AndroidAutoSessionFailed, session.lastError());
    if (VITA_AUTO_PERFORMANCE &&
        perfReporter.report(videoFrames, session, true))
    {
        transport.logDiagnostics("perf-final");
    }
#if VITA_AUTO_VERBOSE_LOGGING
    const inputs::DeliveryStats inputStats = inputs::deliveryStats();
    vitaLogInfo(
        "direct-usb: input dequeued=%u delivered=%u moves_coalesced=%u max_queue_us=%u",
        inputStats.dequeued,
        inputStats.delivered,
        inputStats.movesCoalesced,
        inputStats.maxQueueUs
    );
#endif
    if (VITA_AUTO_VERBOSE_LOGGING || lastError() != AppError::None || session.lastError() != 0)
        vitaLog(
        "direct-usb: session ended state=%d err=%d (%s) tls=0x%08x video=%u",
        static_cast<int>(session.state()),
        session.lastError(),
        session.lastErrorName(),
        session.lastTlsError(),
        videoFrames
    );
    if (VITA_AUTO_VERBOSE_LOGGING || microphoneInput.lastError() != 0)
        vitaLog(
        "direct-usb: microphone captured=%u dropped=%u sent=%u acked=%u err=0x%08x (%s)",
        microphoneInput.capturedFrames(),
        microphoneInput.droppedFrames(),
        session.microphoneFramesSent(),
        session.microphoneAcksReceived(),
        microphoneInput.lastError(), microphoneInput.lastErrorName()
    );
    audio::sink::logStats(
        "direct-usb",
        audioStats,
        mediaAudioOutput,
        promptAudioOutput
    );
    // Stop producers before closing USB. Returning then destroys TLS and all
    // per-session buffers before the thread wrapper publishes completion.
    vitaLogInfo("direct-usb: stopping microphone");
    microphoneInput.stop();
    vitaLogInfo("direct-usb: stopping audio outputs");
    mediaAudioOutput.close();
    promptAudioOutput.close();
    if (VITA_AUTO_VERBOSE_LOGGING || lastError() != AppError::None || session.lastError() != 0)
        transport.logDiagnostics("session-ended");
    closeTransport(transport);
    const int result = lastError() == AppError::None ? 0 : 1;
    if (VITA_AUTO_VERBOSE_LOGGING || result != 0)
        vitaLog("direct-usb: app result=%d error=%d (%s) detail=0x%08x", result,
            static_cast<int>(lastError()), lastErrorName(), lastErrorDetail());
    return result;
}

int directUsbThreadEntry(SceSize, void*)
{
    // Keep sceKernelExitThread outside the scope containing C++ session objects.
    // It does not unwind that scope, which would leak on every reconnect.
    const int result = runDirectSession();
    vitaLogInfo("direct-usb: session objects released");
    __atomic_store_n(&g_directIoFinished, 1u, __ATOMIC_RELEASE);
    return sceKernelExitThread(result);
}

} // namespace

bool start(const Settings& settings)
{
    if (g_directUsbThread >= 0 || !canRestart())
        return false;
    const ConnectionMode mode = settings.connection;
    g_audioOnVita = settings.audioOnVita;
    g_mode = mode;
    std::snprintf(g_host, sizeof(g_host), "%s", settings.host);
    Transport::setWakeHandler(mode == ConnectionMode::Usb ? &UsbTransport::wakeKernel : nullptr);
    __atomic_store_n(&g_stopRequested, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_status, static_cast<int>(Status::WaitingForPhone), __ATOMIC_RELEASE);
    __atomic_store_n(&g_directIoError, static_cast<int>(AppError::None), __ATOMIC_RELEASE);
    __atomic_store_n(&g_directIoErrorDetail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_directIoFinished, 0u, __ATOMIC_RELEASE);
    g_directUsbThread = sceKernelCreateThread(
        "vita-auto-direct-io",
        directUsbThreadEntry,
        DIRECT_USB_THREAD_PRIORITY,
        DIRECT_USB_THREAD_STACK_SIZE,
        0,
        0,
        nullptr
    );
    if (g_directUsbThread < 0) {
        recordAppError(AppError::IoThreadCreateFailed, g_directUsbThread);
        vitaLog(
            "direct-usb: IO thread create failed err=0x%08x",
            g_directUsbThread
        );
        g_directUsbThread = -1;
        return false;
    }

    const int startResult = sceKernelStartThread(
        g_directUsbThread,
        0,
        nullptr
    );
    if (startResult < 0) {
        recordAppError(AppError::IoThreadStartFailed, startResult);
        vitaLog(
            "direct-usb: IO thread start failed err=0x%08x",
            startResult
        );
        sceKernelDeleteThread(g_directUsbThread);
        g_directUsbThread = -1;
        return false;
    }

    vitaLogInfo("direct-usb: IO thread started");
    return true;
}

void requestStop()
{
    // Do not call bridge exports here: cancellation can precede bridge loading.
    // The owner checks this during enumeration and after its bounded USB waits.
    __atomic_store_n(&g_stopRequested, 1u, __ATOMIC_RELEASE);
}

Status status()
{
    return static_cast<Status>(__atomic_load_n(&g_status, __ATOMIC_ACQUIRE));
}

bool canRestart()
{
    return __atomic_load_n(&g_canRestart, __ATOMIC_ACQUIRE) != 0;
}

bool finished()
{
    return __atomic_load_n(&g_directIoFinished, __ATOMIC_ACQUIRE) != 0;
}

void join()
{
    if (g_directUsbThread < 0)
        return;
    sceKernelWaitThreadEnd(g_directUsbThread, nullptr, nullptr);
    sceKernelDeleteThread(g_directUsbThread);
    g_directUsbThread = -1;
}

AppError lastError()
{
    return static_cast<AppError>(__atomic_load_n(&g_directIoError, __ATOMIC_ACQUIRE));
}

const char* lastErrorName() { return appErrorName(lastError()); }

int lastErrorDetail()
{
    return __atomic_load_n(&g_directIoErrorDetail, __ATOMIC_ACQUIRE);
}

} // namespace direct_session
} // namespace app
