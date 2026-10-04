#pragma once

#include <cstddef>
#include <cstdint>

namespace android_auto { class DirectSession; }

namespace logger {
namespace performance {

enum class Metric {
    UsbRx,
    SessionFeed,
    UsbTx,
    KernelFirstAge,
    KernelLastAge,
    KernelBatchSpan,
    RxFirstToParsed,
    RxLastToParsed,
    ParsedToQueue,
    RxFirstToPresent,
    RxLastToPresent,
    ParsedToPresent,
    InputToMedia,
    InputToPresent,
    InputQueue,
    InputEncode,
    QueueWait,
    Decode,
    DecodeLockWait,
    PresentLockWait,
    ReadyWait,
    EndToEnd,
    GxmWait,
    GxmUpload,
    GxmDraw,
    GxmTotal,
    Size
};

enum class Cadence { AaCadence, ArrivalCadence, PresentCadence, Size };
enum class Count { UsbRxBytes, UsbTxBytes, PresentedFrames, TimestampZero, TimestampDiscontinuity, Size };

#if VITA_AUTO_PERFORMANCE
// Atomic aggregates may be recorded by the IO, input, decoder and main threads.
uint64_t nowUs();
// Main thread only, between sessions with every worker stopped.
void reset();
uint32_t elapsedUs(uint64_t startUs);
void record(Metric metric, uint32_t durationUs);
void recordCadence(Cadence cadence, uint32_t intervalUs);
void add(Count counter, uint32_t amount);
void recordQueueDepth(uint32_t depth);

// Gesture trace hooks require verbose logging and serialize shared trace state.
void beginGesture(uint64_t nowUs, uint32_t reportsSent);
void endGesture(uint64_t nowUs, uint32_t reportsSent);
void frameArrived(uint64_t arrivalUs, uint64_t timestampUs, size_t bytes);
void framePresented(uint64_t presentedAtUs);

struct PerfSnapshot {
    uint32_t count = 0;
    uint32_t totalUs = 0;
};

struct CadenceSnapshot {
    uint32_t count = 0;
    uint32_t totalUs = 0;
    uint32_t lateCount = 0;
};

class Reporter {
public:
    // IO thread: report every 300 video frames (10 secs)
    bool report(uint32_t videoFrames, const android_auto::DirectSession& session,
                bool force = false);

private:
    uint64_t lastReportUs = 0;
    uint32_t lastVideoFrames = 0;
    uint32_t lastRxBytes = 0;
    uint32_t lastTxBytes = 0;
    uint32_t lastPresentedFrames = 0;
    uint32_t lastInputDelivered = 0;
    uint32_t lastTouchDelivered = 0;
    uint32_t lastButtonDelivered = 0;
    uint32_t lastRotarySteps = 0;
    uint32_t lastMovesCoalesced = 0;
    PerfSnapshot metricSnapshots[static_cast<size_t>(Metric::Size)]{};
    CadenceSnapshot cadenceSnapshots[static_cast<size_t>(Cadence::Size)]{};
    uint32_t lastTimestampZero = 0;
    uint32_t lastTimestampDiscontinuity = 0;
};

#else
// No profiler state, atomics, or timing calls in a build without profiling.
inline uint64_t nowUs() { return 0; }
inline void reset() {}
inline uint32_t elapsedUs(uint64_t) { return 0; }
inline void record(Metric, uint32_t) {}
inline void recordCadence(Cadence, uint32_t) {}
inline void add(Count, uint32_t) {}
inline void recordQueueDepth(uint32_t) {}
inline void beginGesture(uint64_t, uint32_t) {}
inline void endGesture(uint64_t, uint32_t) {}
inline void frameArrived(uint64_t, uint64_t, size_t) {}
inline void framePresented(uint64_t) {}
class Reporter {
public:
    bool report(uint32_t, const android_auto::DirectSession&, bool = false)
    {
        return false;
    }
};

#endif

} // namespace performance
} // namespace logger
