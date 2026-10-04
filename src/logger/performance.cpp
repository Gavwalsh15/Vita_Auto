#include "logger/performance.h"
#include "app/config.h"
#include "inputs/vita_input.h"
#include "video/video_pipeline.h"
#include "android_auto/aa_direct.h"
#include "logger/vita_log.h"

#include <psp2/kernel/threadmgr.h>
#include <algorithm>

namespace logger {
namespace performance {
namespace {

struct PerfCounter {
    volatile uint32_t count = 0;
    volatile uint32_t totalUs = 0;
    volatile uint32_t maxUs = 0;
};

struct CadenceCounter {
    volatile uint32_t count = 0;
    volatile uint32_t totalUs = 0;
    volatile uint32_t minUs = UINT32_MAX;
    volatile uint32_t maxUs = 0;
    volatile uint32_t lateCount = 0;
};

static PerfCounter g_metrics[static_cast<size_t>(Metric::Size)];
static CadenceCounter g_cadences[static_cast<size_t>(Cadence::Size)];
static volatile uint32_t g_counts[static_cast<size_t>(Count::Size)]{};
static volatile uint32_t g_perfQueueDepthMax = 0;
constexpr uint32_t DIRECT_USB_PERF_REPORT_FRAMES = 300;

PerfCounter& metric(Metric value) { return g_metrics[static_cast<size_t>(value)]; }
CadenceCounter& cadence(Cadence value) { return g_cadences[static_cast<size_t>(value)]; }
volatile uint32_t& count(Count value) { return g_counts[static_cast<size_t>(value)]; }

struct GestureCadence {
    uint64_t lastUs = 0;
    uint64_t totalUs = 0;
    uint32_t count = 0;
    uint32_t minUs = UINT32_MAX;
    uint32_t maxUs = 0;
    // <=50 ms, 50-100 ms, 100-250 ms, >250 ms.
    uint32_t buckets[4] = {};
};

struct GestureTrace {
    bool active = false;
    uint64_t startedUs = 0;
    uint64_t firstFrameArrivalUs = 0;
    uint32_t reportsAtStart = 0;
    uint32_t receivedFrames = 0;
    uint32_t presentedFrames = 0;
    uint32_t receivedBytes = 0;
    uint32_t queueDepthMax = 0;
    GestureCadence aa;
    GestureCadence arrival;
    GestureCadence present;
};

static GestureTrace g_gestureTrace;
static SceUID g_gestureMutex = -1;

class GestureTraceLock {
public:
    GestureTraceLock()
        : locked_(g_gestureMutex >= 0 &&
                  sceKernelLockMutex(g_gestureMutex, 1, nullptr) >= 0) {}
    ~GestureTraceLock()
    {
        if (locked_)
            sceKernelUnlockMutex(g_gestureMutex, 1);
    }
    GestureTraceLock(const GestureTraceLock&) = delete;
    GestureTraceLock& operator=(const GestureTraceLock&) = delete;
    bool locked() const { return locked_; }

private:
    bool locked_;
};

void recordGestureCadence(GestureCadence& cadence, uint64_t sampleUs)
{
    if (sampleUs == 0)
        return;

    if (cadence.lastUs != 0 && sampleUs > cadence.lastUs) {
        const uint64_t rawDelta = sampleUs - cadence.lastUs;
        const uint32_t delta = rawDelta > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(rawDelta);
        cadence.totalUs += delta;
        ++cadence.count;
        if (delta < cadence.minUs)
            cadence.minUs = delta;
        if (delta > cadence.maxUs)
            cadence.maxUs = delta;

        if (delta <= 50000)
            ++cadence.buckets[0];
        else if (delta <= 100000)
            ++cadence.buckets[1];
        else if (delta <= 250000)
            ++cadence.buckets[2];
        else
            ++cadence.buckets[3];
    }
    cadence.lastUs = sampleUs;
}

void updateAtomicMax(volatile uint32_t& target, uint32_t value)
{
    uint32_t observed = __atomic_load_n(&target, __ATOMIC_RELAXED);
    while (value > observed &&
           !__atomic_compare_exchange_n(
               &target,
               &observed,
               value,
               false,
               __ATOMIC_RELAXED,
               __ATOMIC_RELAXED))
    {
    }
}

void recordCounter(PerfCounter& counter, uint32_t durationUs)
{
    __atomic_add_fetch(&counter.count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&counter.totalUs, durationUs, __ATOMIC_RELAXED);

    updateAtomicMax(counter.maxUs, durationUs);
}

uint32_t intervalAverage(
    const PerfCounter& counter,
    PerfSnapshot& previous,
    uint32_t& intervalCount)
{
    const uint32_t count = __atomic_load_n(&counter.count, __ATOMIC_RELAXED);
    const uint32_t total = __atomic_load_n(&counter.totalUs, __ATOMIC_RELAXED);
    intervalCount = count - previous.count;
    const uint32_t intervalTotal = total - previous.totalUs;
    previous.count = count;
    previous.totalUs = total;
    return intervalCount == 0 ? 0 : intervalTotal / intervalCount;
}

uint32_t takeIntervalMax(PerfCounter& counter)
{
    return __atomic_exchange_n(&counter.maxUs, 0, __ATOMIC_RELAXED);
}

void recordCadenceCounter(CadenceCounter& counter, uint32_t intervalUs)
{
    __atomic_add_fetch(&counter.count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&counter.totalUs, intervalUs, __ATOMIC_RELAXED);
    if (intervalUs > 50000)
        __atomic_add_fetch(&counter.lateCount, 1, __ATOMIC_RELAXED);

    uint32_t observedMin = __atomic_load_n(&counter.minUs, __ATOMIC_RELAXED);
    while (intervalUs < observedMin &&
           !__atomic_compare_exchange_n(
               &counter.minUs,
               &observedMin,
               intervalUs,
               false,
               __ATOMIC_RELAXED,
               __ATOMIC_RELAXED))
    {
    }

    updateAtomicMax(counter.maxUs, intervalUs);
}

struct PerfInterval {
    uint32_t count;
    uint32_t averageUs;
    uint32_t maxUs;
};

struct CadenceInterval {
    uint32_t count;
    uint32_t averageUs;
    uint32_t minUs;
    uint32_t maxUs;
    uint32_t lateCount;
};

PerfInterval takePerfInterval(
    PerfCounter& counter,
    PerfSnapshot& previous
)
{
    PerfInterval result{};
    result.averageUs = intervalAverage(counter, previous, result.count);
    result.maxUs = takeIntervalMax(counter);
    return result;
}

CadenceInterval takeCadenceInterval(
    CadenceCounter& counter,
    CadenceSnapshot& previous
)
{
    CadenceInterval result{};
    const uint32_t count = __atomic_load_n(&counter.count, __ATOMIC_RELAXED);
    const uint32_t total = __atomic_load_n(&counter.totalUs, __ATOMIC_RELAXED);
    const uint32_t late = __atomic_load_n(&counter.lateCount, __ATOMIC_RELAXED);
    result.count = count - previous.count;
    const uint32_t intervalTotal = total - previous.totalUs;
    result.averageUs = result.count == 0 ? 0 : intervalTotal / result.count;
    result.minUs = __atomic_exchange_n(
        &counter.minUs,
        UINT32_MAX,
        __ATOMIC_RELAXED
    );
    if (result.minUs == UINT32_MAX)
        result.minUs = 0;
    result.maxUs = __atomic_exchange_n(&counter.maxUs, 0, __ATOMIC_RELAXED);
    result.lateCount = late - previous.lateCount;
    previous.count = count;
    previous.totalUs = total;
    previous.lateCount = late;
    return result;
}

} // namespace

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

void reset()
{
    // Reuse one process-lifetime mutex; reset runs with all workers stopped.
    if (VITA_AUTO_VERBOSE_LOGGING && g_gestureMutex < 0) {
        g_gestureMutex = sceKernelCreateMutex("vita-auto-gesture", 0, 0, nullptr);
        if (g_gestureMutex < 0)
            vitaLog("perf.gesture disabled: mutex creation failed err=0x%08x", g_gestureMutex);
    }
    std::fill(std::begin(g_metrics), std::end(g_metrics), PerfCounter{});
    std::fill(std::begin(g_cadences), std::end(g_cadences), CadenceCounter{});
    std::fill(std::begin(g_counts), std::end(g_counts), 0);
    g_perfQueueDepthMax = 0;
    g_gestureTrace = GestureTrace{};
}

uint32_t elapsedUs(uint64_t startUs)
{
    const uint64_t elapsed = sceKernelGetSystemTimeWide() - startUs;
    return elapsed > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed);
}

void record(Metric value, uint32_t durationUs)
{
    recordCounter(metric(value), durationUs);
}

void recordCadence(Cadence value, uint32_t intervalUs)
{
    recordCadenceCounter(cadence(value), intervalUs);
}

void add(Count value, uint32_t amount)
{
    __atomic_add_fetch(&count(value), amount, __ATOMIC_RELAXED);
}

void recordQueueDepth(uint32_t depth)
{
    updateAtomicMax(g_perfQueueDepthMax, depth);
    if (!VITA_AUTO_VERBOSE_LOGGING)
        return;
    const GestureTraceLock lock;
    if (!lock.locked())
        return;
    if (g_gestureTrace.active && depth > g_gestureTrace.queueDepthMax)
        g_gestureTrace.queueDepthMax = depth;
}

void beginGesture(uint64_t nowUs, uint32_t reportsSent)
{
    if (!VITA_AUTO_VERBOSE_LOGGING)
        return;

    const GestureTraceLock lock;
    if (!lock.locked())
        return;
    g_gestureTrace = {};
    g_gestureTrace.active = true;
    g_gestureTrace.startedUs = nowUs;
    g_gestureTrace.reportsAtStart = reportsSent;
}

void endGesture(uint64_t nowUs, uint32_t reportsSent)
{
    if (!VITA_AUTO_VERBOSE_LOGGING)
        return;

    GestureTrace trace;
    {
        const GestureTraceLock lock;
        if (!lock.locked() || !g_gestureTrace.active)
            return;
        trace = g_gestureTrace;
        g_gestureTrace.active = false;
    }

    const uint32_t durationMs = static_cast<uint32_t>(
        (nowUs - trace.startedUs) / 1000
    );
    const bool sawFrame = trace.firstFrameArrivalUs != 0;
    const uint32_t firstFrameMs = sawFrame
        ? static_cast<uint32_t>(
            (trace.firstFrameArrivalUs - trace.startedUs) /
            1000
        )
        : 0;
    vitaLog(
        "perf.gesture ms=%u reports=%u rx=%u/%uKiB shown=%u first=%s%u qmax=%u",
        durationMs,
        reportsSent - trace.reportsAtStart,
        trace.receivedFrames,
        trace.receivedBytes / 1024,
        trace.presentedFrames,
        sawFrame ? "" : "none/",
        firstFrameMs,
        trace.queueDepthMax
    );

    const GestureCadence& aa = trace.aa;
    const GestureCadence& arrival = trace.arrival;
    const GestureCadence& present = trace.present;
    vitaLog(
        "perf.gesture.cadence aa=%u/%u/%u [%u,%u,%u,%u] arrival=%u/%u/%u [%u,%u,%u,%u] present=%u/%u/%u [%u,%u,%u,%u]",
        aa.count == 0 ? 0 : static_cast<uint32_t>(aa.totalUs / aa.count),
        aa.minUs == UINT32_MAX ? 0 : aa.minUs,
        aa.maxUs,
        aa.buckets[0], aa.buckets[1], aa.buckets[2], aa.buckets[3],
        arrival.count == 0
            ? 0
            : static_cast<uint32_t>(arrival.totalUs / arrival.count),
        arrival.minUs == UINT32_MAX ? 0 : arrival.minUs,
        arrival.maxUs,
        arrival.buckets[0], arrival.buckets[1],
        arrival.buckets[2], arrival.buckets[3],
        present.count == 0
            ? 0
            : static_cast<uint32_t>(present.totalUs / present.count),
        present.minUs == UINT32_MAX ? 0 : present.minUs,
        present.maxUs,
        present.buckets[0], present.buckets[1],
        present.buckets[2], present.buckets[3]
    );
}

void frameArrived(uint64_t arrivalUs, uint64_t timestampUs, size_t bytes)
{
    if (!VITA_AUTO_VERBOSE_LOGGING)
        return;
    const GestureTraceLock lock;
    if (!lock.locked() || !g_gestureTrace.active)
        return;
    if (g_gestureTrace.firstFrameArrivalUs == 0)
        g_gestureTrace.firstFrameArrivalUs = arrivalUs;
    ++g_gestureTrace.receivedFrames;
    const uint64_t totalBytes = static_cast<uint64_t>(g_gestureTrace.receivedBytes) + bytes;
    g_gestureTrace.receivedBytes = totalBytes > UINT32_MAX
        ? UINT32_MAX : static_cast<uint32_t>(totalBytes);
    recordGestureCadence(g_gestureTrace.arrival, arrivalUs);
    recordGestureCadence(g_gestureTrace.aa, timestampUs);
}

void framePresented(uint64_t presentedAtUs)
{
    if (!VITA_AUTO_VERBOSE_LOGGING)
        return;
    const GestureTraceLock lock;
    if (!lock.locked() || !g_gestureTrace.active)
        return;
    ++g_gestureTrace.presentedFrames;
    recordGestureCadence(g_gestureTrace.present, presentedAtUs);
}

bool Reporter::report(
    uint32_t videoFrames,
    const android_auto::DirectSession& session,
    bool force
)
{
    const uint64_t nowUs = sceKernelGetSystemTimeWide();
    if (this->lastReportUs == 0) {
        const inputs::DeliveryStats inputStats = inputs::deliveryStats();
        this->lastReportUs = nowUs;
        this->lastVideoFrames = videoFrames;
        this->lastRxBytes = __atomic_load_n(&count(Count::UsbRxBytes), __ATOMIC_RELAXED);
        this->lastTxBytes = __atomic_load_n(&count(Count::UsbTxBytes), __ATOMIC_RELAXED);
        this->lastPresentedFrames = __atomic_load_n(
            &count(Count::PresentedFrames),
            __ATOMIC_RELAXED
        );
        this->lastInputDelivered = inputStats.delivered;
        this->lastTouchDelivered = inputStats.touchDelivered;
        this->lastButtonDelivered = inputStats.buttonDelivered;
        this->lastRotarySteps = inputStats.rotarySteps;
        this->lastMovesCoalesced = inputStats.movesCoalesced;
        this->lastTimestampZero = __atomic_load_n(
            &count(Count::TimestampZero),
            __ATOMIC_RELAXED
        );
        this->lastTimestampDiscontinuity = __atomic_load_n(
            &count(Count::TimestampDiscontinuity),
            __ATOMIC_RELAXED
        );
        return false;
    }

    if (!force &&
        videoFrames - this->lastVideoFrames < DIRECT_USB_PERF_REPORT_FRAMES)
        return false;

    const uint32_t elapsed = static_cast<uint32_t>(nowUs - this->lastReportUs);
    const uint32_t frameDelta = videoFrames - this->lastVideoFrames;
    const uint32_t fpsTimes10 = elapsed == 0 ? 0 : static_cast<uint32_t>(
        (static_cast<uint64_t>(frameDelta) * 10000000ULL) / elapsed
    );

    const uint32_t rxBytes = __atomic_load_n(&count(Count::UsbRxBytes), __ATOMIC_RELAXED);
    const uint32_t txBytes = __atomic_load_n(&count(Count::UsbTxBytes), __ATOMIC_RELAXED);
    const uint32_t presented = __atomic_load_n(
        &count(Count::PresentedFrames),
        __ATOMIC_RELAXED
    );

    auto takeMetric = [this](Metric value) {
        return takePerfInterval(metric(value), metricSnapshots[static_cast<size_t>(value)]);
    };
    auto takeCadence = [this](Cadence value) {
        return takeCadenceInterval(cadence(value), cadenceSnapshots[static_cast<size_t>(value)]);
    };

    const PerfInterval rx = takeMetric(Metric::UsbRx);
    const PerfInterval feed = takeMetric(Metric::SessionFeed);
    const PerfInterval tx = takeMetric(Metric::UsbTx);
    const PerfInterval kernelFirstAge = takeMetric(Metric::KernelFirstAge);
    const PerfInterval kernelLastAge = takeMetric(Metric::KernelLastAge);
    const PerfInterval kernelBatchSpan = takeMetric(Metric::KernelBatchSpan);
    const PerfInterval rxFirstToParsed = takeMetric(Metric::RxFirstToParsed);
    const PerfInterval rxLastToParsed = takeMetric(Metric::RxLastToParsed);
    const PerfInterval parsedToQueue = takeMetric(Metric::ParsedToQueue);
    const PerfInterval rxFirstToPresent = takeMetric(Metric::RxFirstToPresent);
    const PerfInterval rxLastToPresent = takeMetric(Metric::RxLastToPresent);
    const PerfInterval parsedToPresent = takeMetric(Metric::ParsedToPresent);
    const PerfInterval inputToMedia = takeMetric(Metric::InputToMedia);
    const PerfInterval inputToPresent = takeMetric(Metric::InputToPresent);
    const PerfInterval inputQueue = takeMetric(Metric::InputQueue);
    const PerfInterval inputEncode = takeMetric(Metric::InputEncode);
    const PerfInterval queue = takeMetric(Metric::QueueWait);
    const PerfInterval decode = takeMetric(Metric::Decode);
    const PerfInterval decodeLock = takeMetric(Metric::DecodeLockWait);
    const PerfInterval presentLock = takeMetric(Metric::PresentLockWait);
    const PerfInterval ready = takeMetric(Metric::ReadyWait);
    const PerfInterval endToEnd = takeMetric(Metric::EndToEnd);
    const PerfInterval gxmWait = takeMetric(Metric::GxmWait);
    const PerfInterval gxmUpload = takeMetric(Metric::GxmUpload);
    const PerfInterval gxmDraw = takeMetric(Metric::GxmDraw);
    const PerfInterval gxmTotal = takeMetric(Metric::GxmTotal);
    const CadenceInterval aaCadence = takeCadence(Cadence::AaCadence);
    const CadenceInterval arrivalCadence = takeCadence(Cadence::ArrivalCadence);
    const CadenceInterval presentCadence = takeCadence(Cadence::PresentCadence);

    const uint32_t intervalRxBytes = rxBytes - this->lastRxBytes;
    const uint32_t intervalTxBytes = txBytes - this->lastTxBytes;
    const uint32_t intervalPresented = presented - this->lastPresentedFrames;
    const uint32_t timestampZero = __atomic_load_n(
        &count(Count::TimestampZero),
        __ATOMIC_RELAXED
    );
    const uint32_t timestampDiscontinuity = __atomic_load_n(
        &count(Count::TimestampDiscontinuity),
        __ATOMIC_RELAXED
    );
    const uint32_t intervalTimestampZero =
        timestampZero - this->lastTimestampZero;
    const uint32_t intervalTimestampDiscontinuity =
        timestampDiscontinuity - this->lastTimestampDiscontinuity;
    const uint32_t intervalQueueDepthMax = __atomic_exchange_n(
        &g_perfQueueDepthMax,
        0,
        __ATOMIC_RELAXED
    );

    vitaLog(
        "perf.io fps=%u.%u rx=%uKiB calls=%u us=%u/%u feed=%u/%u tx=%uKiB calls=%u us=%u/%u",
        fpsTimes10 / 10,
        fpsTimes10 % 10,
        intervalRxBytes / 1024,
        rx.count,
        rx.averageUs,
        rx.maxUs,
        feed.averageUs,
        feed.maxUs,
        intervalTxBytes / 1024,
        tx.count,
        tx.averageUs,
        tx.maxUs
    );
    vitaLog(
        "perf.video presented=%u queue=%u/%u decode=%u/%u dlock=%u/%u plock=%u/%u ready=%u/%u e2e=%u/%u peak=%u",
        intervalPresented,
        queue.averageUs,
        queue.maxUs,
        decode.averageUs,
        decode.maxUs,
        decodeLock.averageUs,
        decodeLock.maxUs,
        presentLock.averageUs,
        presentLock.maxUs,
        ready.averageUs,
        ready.maxUs,
        endToEnd.averageUs,
        endToEnd.maxUs,
        video::pipeline::peakQueueDepth()
    );
    vitaLog(
        "perf.pipe krx_old=%u/%u krx_new=%u/%u span=%u/%u parse_old=%u/%u parse_new=%u/%u parsed_queue=%u/%u us",
        kernelFirstAge.averageUs,
        kernelFirstAge.maxUs,
        kernelLastAge.averageUs,
        kernelLastAge.maxUs,
        kernelBatchSpan.averageUs,
        kernelBatchSpan.maxUs,
        rxFirstToParsed.averageUs,
        rxFirstToParsed.maxUs,
        rxLastToParsed.averageUs,
        rxLastToParsed.maxUs,
        parsedToQueue.averageUs,
        parsedToQueue.maxUs
    );
    vitaLog(
        "perf.pipe.present rx_old=%u/%u rx_new=%u/%u parsed=%u/%u input_media=%u/%u input_present=%u/%u us",
        rxFirstToPresent.averageUs,
        rxFirstToPresent.maxUs,
        rxLastToPresent.averageUs,
        rxLastToPresent.maxUs,
        parsedToPresent.averageUs,
        parsedToPresent.maxUs,
        inputToMedia.averageUs,
        inputToMedia.maxUs,
        inputToPresent.averageUs,
        inputToPresent.maxUs
    );
    vitaLog(
        "perf.gxm frames=%u wait=%u/%u upload=%u/%u draw=%u/%u total=%u/%u (avg/max us)",
        gxmTotal.count,
        gxmWait.averageUs,
        gxmWait.maxUs,
        gxmUpload.averageUs,
        gxmUpload.maxUs,
        gxmDraw.averageUs,
        gxmDraw.maxUs,
        gxmTotal.averageUs,
        gxmTotal.maxUs
    );
    vitaLog(
        "perf.cadence aa=%u/%u/%u arrival=%u/%u/%u present=%u/%u/%u late=%u/%u/%u ts0=%u disc=%u qmax=%u (avg/min/max us)",
        aaCadence.averageUs,
        aaCadence.minUs,
        aaCadence.maxUs,
        arrivalCadence.averageUs,
        arrivalCadence.minUs,
        arrivalCadence.maxUs,
        presentCadence.averageUs,
        presentCadence.minUs,
        presentCadence.maxUs,
        aaCadence.lateCount,
        arrivalCadence.lateCount,
        presentCadence.lateCount,
        intervalTimestampZero,
        intervalTimestampDiscontinuity,
        intervalQueueDepthMax
    );
    const android_auto::MediaAckStats ack = session.mediaAckStats();
    const uint32_t ackSubmitAverageUs = ack.submitSamples == 0
        ? 0
        : static_cast<uint32_t>(
            ack.submitLatencyTotalUs / ack.submitSamples
        );
    vitaLog(
        "perf.ack queued=%u submitted=%u pending=%u batches=%u delay=%u/%u us",
        ack.queued,
        ack.submitted,
        ack.pending,
        ack.submitSamples,
        ackSubmitAverageUs,
        ack.submitLatencyMaxUs
    );
    const inputs::DeliveryStats inputStats = inputs::deliveryStats();
    vitaLog(
        "perf.input queue=%u/%u encode=%u/%u events=%u sent=%u touch=%u buttons=%u rotary=%u coalesced=%u (interval, avg/max us)",
        inputQueue.averageUs,
        inputQueue.maxUs,
        inputEncode.averageUs,
        inputEncode.maxUs,
        inputQueue.count,
        inputStats.delivered - this->lastInputDelivered,
        inputStats.touchDelivered - this->lastTouchDelivered,
        inputStats.buttonDelivered - this->lastButtonDelivered,
        inputStats.rotarySteps - this->lastRotarySteps,
        inputStats.movesCoalesced - this->lastMovesCoalesced
    );

    this->lastReportUs = nowUs;
    this->lastVideoFrames = videoFrames;
    this->lastRxBytes = rxBytes;
    this->lastTxBytes = txBytes;
    this->lastPresentedFrames = presented;
    this->lastInputDelivered = inputStats.delivered;
    this->lastTouchDelivered = inputStats.touchDelivered;
    this->lastButtonDelivered = inputStats.buttonDelivered;
    this->lastRotarySteps = inputStats.rotarySteps;
    this->lastMovesCoalesced = inputStats.movesCoalesced;
    this->lastTimestampZero = timestampZero;
    this->lastTimestampDiscontinuity = timestampDiscontinuity;
    return true;
}

} // namespace performance
} // namespace logger
