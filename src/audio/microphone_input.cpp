#include "logger/vita_log.h"
#include "audio/microphone_input.h"

#include <psp2/audioin.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

namespace audio {

namespace {

constexpr int MICROPHONE_SAMPLE_RATE = 16000;
constexpr int MICROPHONE_THREAD_PRIORITY = 0x10000100;
constexpr int MICROPHONE_THREAD_STACK_SIZE = 0x10000;

} // namespace

void MicrophoneInput::setError(app::AppError error, int detail)
{
    const bool changed = lastAppError() != error || lastError() != detail;
    __atomic_store_n(&lastError_, detail, __ATOMIC_RELEASE);
    __atomic_store_n(&lastAppError_, static_cast<int>(error), __ATOMIC_RELEASE);
    // Retry loops report a new failure once, keeping file IO out of every retry.
    if (changed && error != app::AppError::None)
        vitaLogAppError(error, detail);
}

MicrophoneInput::~MicrophoneInput()
{
    stop();
}

int MicrophoneInput::threadEntry(SceSize args, void* argp)
{
    if (args != sizeof(MicrophoneInput*) || argp == nullptr)
        return sceKernelExitThread(0);

    MicrophoneInput* self = *static_cast<MicrophoneInput**>(argp);
    if (self != nullptr)
        self->run();

    return sceKernelExitThread(0);
}

bool MicrophoneInput::ensureWorker()
{
    if (thread_ >= 0)
        return true;

    enabledSema_ = sceKernelCreateSema(
        "vita-auto-mic-ready",
        0,
        0,
        1,
        nullptr
    );
    if (enabledSema_ < 0) {
        setError(app::AppError::MicrophoneSemaphoreCreateFailed, enabledSema_);
        return false;
    }

    thread_ = sceKernelCreateThread(
        "vita-auto-mic",
        threadEntry,
        MICROPHONE_THREAD_PRIORITY,
        MICROPHONE_THREAD_STACK_SIZE,
        0,
        0,
        nullptr
    );
    if (thread_ < 0) {
        setError(app::AppError::MicrophoneThreadCreateFailed, thread_);
        sceKernelDeleteSema(enabledSema_);
        enabledSema_ = -1;
        return false;
    }

    __atomic_store_n(&running_, 1u, __ATOMIC_RELEASE);
    MicrophoneInput* self = this;
    const int result = sceKernelStartThread(thread_, sizeof(self), &self);
    if (result < 0) {
        setError(app::AppError::MicrophoneThreadStartFailed, result);
        __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);
        sceKernelDeleteThread(thread_);
        sceKernelDeleteSema(enabledSema_);
        thread_ = -1;
        enabledSema_ = -1;
        return false;
    }

    return true;
}

bool MicrophoneInput::setEnabled(bool enabled)
{
    if (enabled && !ensureWorker())
        return false;

    const uint32_t generation = __atomic_load_n(&generation_, __ATOMIC_ACQUIRE);
    if (((generation & 1u) != 0) != enabled) {
        __atomic_store_n(&generation_, generation + 1u, __ATOMIC_RELEASE);
        if (enabled)
            sceKernelSignalSema(enabledSema_, 1);
    }
    return true;
}

bool MicrophoneInput::pop(Frame& frame)
{
    const uint32_t generation = __atomic_load_n(&generation_, __ATOMIC_ACQUIRE);
    QueuedFrame queued;
    while (queue_.pop(queued)) {
        if ((generation & 1u) != 0 && queued.generation == generation) {
            frame = queued.frame;
            return true;
        }
    }
    return false;
}

void MicrophoneInput::run()
{
    while (__atomic_load_n(&running_, __ATOMIC_ACQUIRE) != 0) {
        const uint32_t captureGeneration = __atomic_load_n(
            &generation_, __ATOMIC_ACQUIRE);
        if ((captureGeneration & 1u) == 0) {
            if (port_ >= 0) {
                sceAudioInReleasePort(port_);
                port_ = -1;
            }
            sceKernelWaitSema(enabledSema_, 1, nullptr);
            continue;
        }

        if (port_ < 0) {
            port_ = sceAudioInOpenPort(
                SCE_AUDIO_IN_PORT_TYPE_VOICE,
                static_cast<int>(SAMPLES_PER_FRAME),
                MICROPHONE_SAMPLE_RATE,
                SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO
            );
            if (port_ < 0) {
                setError(app::AppError::MicrophonePortOpenFailed, port_);
                sceKernelDelayThread(100000);
                continue;
            }
            setError(app::AppError::None, 0);
        }

        QueuedFrame captured;
        captured.generation = captureGeneration;
        Frame& frame = captured.frame;
        const int result = sceAudioInInput(port_, frame.samples.data());
        frame.capturedAtUs = sceKernelGetSystemTimeWide();
        if (result < 0) {
            setError(app::AppError::MicrophoneCaptureFailed, result);
            sceAudioInReleasePort(port_);
            port_ = -1;
            sceKernelDelayThread(100000);
            continue;
        }

        if (__atomic_load_n(&generation_, __ATOMIC_ACQUIRE) != captureGeneration)
        {
            continue;
        }

        if (queue_.push(captured)) {
            __atomic_add_fetch(&capturedFrames_, 1u, __ATOMIC_RELAXED);
            if (wake_ != nullptr)
                wake_();
        } else {
            __atomic_add_fetch(&droppedFrames_, 1u, __ATOMIC_RELAXED);
        }
    }

    if (port_ >= 0) {
        sceAudioInReleasePort(port_);
        port_ = -1;
    }
}

void MicrophoneInput::stop()
{
    setEnabled(false);
    __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);

    if (enabledSema_ >= 0)
        sceKernelSignalSema(enabledSema_, 1);

    if (thread_ >= 0) {
        sceKernelWaitThreadEnd(thread_, nullptr, nullptr);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
    }

    if (port_ >= 0) {
        sceAudioInReleasePort(port_);
        port_ = -1;
    }

    if (enabledSema_ >= 0) {
        sceKernelDeleteSema(enabledSema_);
        enabledSema_ = -1;
    }

    queue_.reset();
}

} // namespace audio
