#pragma once

#include "app/app_errors.h"

#include <psp2/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

class Output {
public:
    Output(uint32_t sampleRate, uint8_t channels)
        : sampleRate_(sampleRate), channels_(channels) {}
    ~Output();

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    bool pushPcm16(const int16_t* samples, size_t sampleCount);

    void close();

    int lastError() const { return __atomic_load_n(&lastError_, __ATOMIC_ACQUIRE); }
    app::AppError lastAppError() const
    {
        return static_cast<app::AppError>(__atomic_load_n(&lastAppError_, __ATOMIC_ACQUIRE));
    }
    const char* lastErrorName() const { return app::appErrorName(lastAppError()); }

private:
    void setError(app::AppError error, int detail);
    static constexpr size_t QUEUE_CAPACITY_SAMPLES = 9600;

    static int threadEntry(SceSize args, void* argp);

    bool ensureWorker();
    bool ensurePort();
    void releaseSyncObjects();
    bool popFrame(std::vector<int16_t>& output);
    void run();

    int port_ = -1;
    const uint32_t sampleRate_;
    const uint8_t channels_;
    std::array<int16_t, QUEUE_CAPACITY_SAMPLES> queue_{};
    size_t queueRead_ = 0;
    size_t queueWrite_ = 0;
    size_t queueSamples_ = 0;
    SceUID thread_ = -1;
    SceUID mutex_ = -1;
    SceUID dataReady_ = -1;
    uint32_t running_ = 0;
    volatile int lastError_ = 0;
    volatile int lastAppError_ = 0;
};

} // namespace audio
