#include "logger/vita_log.h"
#include "audio_output.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <cstring>

namespace audio {

namespace {

constexpr size_t AUDIO_GRAIN = 512;
constexpr int AUDIO_THREAD_PRIORITY = 0x10000100;
constexpr int AUDIO_THREAD_STACK_SIZE = 0x10000;

} // namespace

void Output::setError(app::AppError error, int detail)
{
    const bool changed = lastAppError() != error || lastError() != detail;
    __atomic_store_n(&lastError_, detail, __ATOMIC_RELEASE);
    __atomic_store_n(&lastAppError_, static_cast<int>(error), __ATOMIC_RELEASE);
    // Retry loops report a new failure once, keeping file IO out of every retry.
    if (changed && error != app::AppError::None)
        vitaLogAppError(error, detail);
}

Output::~Output()
{
    close();
}

bool Output::ensurePort()
{
    if (port_ >= 0)
        return true;

    port_ = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_BGM,
        static_cast<int>(AUDIO_GRAIN),
        static_cast<int>(sampleRate_),
        channels_ == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO
    );

    if (port_ < 0) {
        setError(app::AppError::AudioPortOpenFailed, port_);
        return false;
    }

    return true;
}

int Output::threadEntry(SceSize args, void* argp)
{
    if (args != sizeof(Output*) || argp == nullptr)
        return sceKernelExitThread(0);

    Output* self = *static_cast<Output**>(argp);
    if (self != nullptr)
        self->run();

    return sceKernelExitThread(0);
}

bool Output::ensureWorker()
{
    if (thread_ >= 0)
        return true;

    if (sampleRate_ == 0 || (channels_ != 1 && channels_ != 2)) {
        setError(app::AppError::AudioFormatUnsupported, -1);
        return false;
    }

    mutex_ = sceKernelCreateMutex("vita-auto-audio", 0, 0, nullptr);
    if (mutex_ < 0) {
        setError(app::AppError::AudioMutexCreateFailed, mutex_);
        return false;
    }

    dataReady_ = sceKernelCreateSema("vita-auto-audio-ready", 0, 0, 1, nullptr);
    if (dataReady_ < 0) {
        setError(app::AppError::AudioSemaphoreCreateFailed, dataReady_);
        releaseSyncObjects();
        return false;
    }

    thread_ = sceKernelCreateThread(
        "vita-auto-audio",
        threadEntry,
        AUDIO_THREAD_PRIORITY,
        AUDIO_THREAD_STACK_SIZE,
        0,
        0,
        nullptr
    );

    if (thread_ < 0) {
        setError(app::AppError::AudioThreadCreateFailed, thread_);
        releaseSyncObjects();
        return false;
    }

    __atomic_store_n(&running_, 1u, __ATOMIC_RELEASE);
    Output* self = this;
    const int startResult = sceKernelStartThread(thread_, sizeof(self), &self);

    if (startResult < 0) {
        setError(app::AppError::AudioThreadStartFailed, startResult);
        __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
        releaseSyncObjects();
        return false;
    }

    return true;
}

bool Output::popFrame(std::vector<int16_t>& output)
{
    sceKernelLockMutex(mutex_, 1, nullptr);

    const size_t frameSamples = AUDIO_GRAIN * channels_;

    const size_t targetSamples = std::max(
        frameSamples,
        (static_cast<size_t>(sampleRate_) * channels_ * bufferMs_) / 1000u
    );

    if (buffering_) {
        if (queueSamples_ < targetSamples) {
            sceKernelUnlockMutex(mutex_, 1);
            return false;
        }
        buffering_ = false;
    }

    if (queueSamples_ < frameSamples) {
        // Ran dry: wait for the buffer to refill instead of stuttering.
        buffering_ = true;
        __atomic_add_fetch(&underruns_, 1u, __ATOMIC_RELEASE);
        sceKernelUnlockMutex(mutex_, 1);
        return false;
    }

    const size_t first = std::min(frameSamples, queue_.size() - queueRead_);
    std::memcpy(output.data(), queue_.data() + queueRead_, first * sizeof(int16_t));
    if (first < frameSamples) {
        std::memcpy(
            output.data() + first,
            queue_.data(),
            (frameSamples - first) * sizeof(int16_t)
        );
    }
    queueRead_ = (queueRead_ + frameSamples) % queue_.size();
    queueSamples_ -= frameSamples;

    sceKernelUnlockMutex(mutex_, 1);
    return true;
}

void Output::run()
{
    std::vector<int16_t> output(AUDIO_GRAIN * channels_);

    while (__atomic_load_n(&running_, __ATOMIC_ACQUIRE) != 0) {
        sceKernelWaitSema(dataReady_, 1, nullptr);

        while (__atomic_load_n(&running_, __ATOMIC_ACQUIRE) != 0) {
            if (!popFrame(output))
                break;

            if (!ensurePort())
                break;

            const int result = sceAudioOutOutput(port_, output.data());
            if (result < 0) {
                setError(app::AppError::AudioOutputFailed, result);
                break;
            }
        }
    }
}

bool Output::pushPcm16(const int16_t* samples, size_t sampleCount)
{
    if (samples == nullptr || sampleCount == 0)
        return true;

    if (!ensureWorker())
        return false;

    const size_t queueLimit = queue_.size();

    sceKernelLockMutex(mutex_, 1, nullptr);

    if (sampleCount > queueLimit) {
        samples += sampleCount - queueLimit;
        sampleCount = queueLimit;
    }

    if (queueSamples_ + sampleCount > queueLimit) {
        const size_t discard = queueSamples_ + sampleCount - queueLimit;
        queueRead_ = (queueRead_ + discard) % queue_.size();
        queueSamples_ -= discard;
    }

    const size_t first = std::min(sampleCount, queue_.size() - queueWrite_);
    std::memcpy(queue_.data() + queueWrite_, samples, first * sizeof(int16_t));
    if (first < sampleCount) {
        std::memcpy(
            queue_.data(),
            samples + first,
            (sampleCount - first) * sizeof(int16_t)
        );
    }
    queueWrite_ = (queueWrite_ + sampleCount) % queue_.size();
    queueSamples_ += sampleCount;

    sceKernelUnlockMutex(mutex_, 1);
    sceKernelSignalSema(dataReady_, 1);
    return true;
}

void Output::releaseSyncObjects()
{
    if (dataReady_ >= 0)
        sceKernelDeleteSema(dataReady_);
    if (mutex_ >= 0)
        sceKernelDeleteMutex(mutex_);

    dataReady_ = -1;
    mutex_ = -1;
}

void Output::close()
{
    if (thread_ >= 0) {
        __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);
        if (dataReady_ >= 0)
            sceKernelSignalSema(dataReady_, 1);
        sceKernelWaitThreadEnd(thread_, nullptr, nullptr);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
    }

    if (port_ >= 0) {
        sceAudioOutOutput(port_, nullptr);
        sceAudioOutReleasePort(port_);
        port_ = -1;
    }

    releaseSyncObjects();
    queueRead_ = 0;
    queueWrite_ = 0;
    queueSamples_ = 0;
    buffering_ = true;
}

} // namespace audio
