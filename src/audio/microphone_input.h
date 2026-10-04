#pragma once

#include "app/app_errors.h"

#include <psp2/types.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/spsc_event_queue.h"

namespace audio {

// A blocking worker captures microphone frames into a shared queue.
// The USB thread consumes them and retains ownership of protocol state.
class MicrophoneInput {
public:
    static constexpr size_t SAMPLES_PER_FRAME = 512;

    struct Frame {
        std::array<int16_t, SAMPLES_PER_FRAME> samples{};
        uint64_t capturedAtUs = 0;
    };

    using WakeFunction = void (*)();

    explicit MicrophoneInput(WakeFunction wake) : wake_(wake) {}
    ~MicrophoneInput();

    MicrophoneInput(const MicrophoneInput&) = delete;
    MicrophoneInput& operator=(const MicrophoneInput&) = delete;

    // The IO owner serializes activation, consumption, and stop.
    bool setEnabled(bool enabled);
    bool pop(Frame& frame);
    void stop();

    int lastError() const { return __atomic_load_n(&lastError_, __ATOMIC_ACQUIRE); }
    app::AppError lastAppError() const
    {
        return static_cast<app::AppError>(__atomic_load_n(&lastAppError_, __ATOMIC_ACQUIRE));
    }
    const char* lastErrorName() const { return app::appErrorName(lastAppError()); }
    uint32_t capturedFrames() const
    {
        return __atomic_load_n(&capturedFrames_, __ATOMIC_ACQUIRE);
    }
    uint32_t droppedFrames() const
    {
        return __atomic_load_n(&droppedFrames_, __ATOMIC_ACQUIRE);
    }

private:
    struct QueuedFrame {
        Frame frame;
        uint32_t generation = 0;
    };

    void setError(app::AppError error, int detail);
    static int threadEntry(SceSize args, void* argp);

    bool ensureWorker();
    void run();

    // Seven usable slots provide 224 ms of elasticity at 16 kHz without
    // allowing a stalled phone ACK to create an unbounded speech delay.
    common::SpscEventQueue<QueuedFrame, 8> queue_;
    SceUID thread_ = -1;
    SceUID enabledSema_ = -1;
    int port_ = -1;
    volatile uint32_t running_ = 0;
    // Odd generations capture; even generations are disabled.
    volatile uint32_t generation_ = 0;
    volatile int lastError_ = 0;
    volatile int lastAppError_ = 0;
    volatile uint32_t capturedFrames_ = 0;
    volatile uint32_t droppedFrames_ = 0;
    const WakeFunction wake_;
};

} // namespace audio
