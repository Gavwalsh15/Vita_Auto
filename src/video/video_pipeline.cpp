#include "video/video_pipeline.h"
#include "app/config.h"
#include "logger/performance.h"
#include "video/video_presenter.h"
#include "android_auto/aa_direct.h"
#include "logger/vita_log.h"
#include "video/gxm_scaler.h"
#include "video/h264_annexb.h"
#include "video/vita_h264_decoder.h"

#include <psp2/kernel/threadmgr.h>
#include <deque>
#include <vector>
#include <utility>

namespace video {
namespace pipeline {
namespace performance = logger::performance;

namespace {

// IO produces compressed units the worker decodes in order. The main thread
// shares only the latest decoded picture, protected by g_decodedFrameMutex.
struct VideoPipelineTiming {
    uint64_t rxFirstCompletedAtUs = 0;
    uint64_t rxLastCompletedAtUs = 0;
    uint64_t parsedAtUs = 0;
    uint64_t queuedAtUs = 0;
    uint64_t inputSubmittedAtUs = 0;
};
struct PendingVideoAccessUnit {
    std::vector<uint8_t> data;
    VideoPipelineTiming timing;
};
// Compressed units remain ordered skipping reference frames breaks later ones.
static std::deque<PendingVideoAccessUnit> g_pendingVideoAccessUnits;
static volatile uint32_t g_pendingVideoAccessUnitCount = 0;
static uint32_t g_peakPendingVideoAccessUnits = 0;
static uint32_t g_lastLoggedQueuePeak = 0;
static bool g_loggedVideoParameterSets = false;
static bool g_loggedFirstDecodedFrame = false;
static uint32_t g_workerDecodedAccessUnits = 0;
// Queue mutex, picture mutex and wake semaphore have separate ownership roles.
static SceUID g_videoMutex = -1;
static SceUID g_decodedFrameMutex = -1;
static SceUID g_videoReadySema = -1;
static SceUID g_videoThread = -1;
static volatile bool g_videoThreadRunning = false;
static volatile uint32_t g_decodedFrameSequence = 0;
static uint32_t g_presentedFrameSequence = 0;
static uint32_t g_hasPresentedFrame = 0;
static int g_failureCode = 0;
static int g_failureDetail = 0;

// Startup failure is IO-owned; after startup only the decoder can fail.
void recordVideoFailure(app::AppError error, int detail)
{
    if (__atomic_load_n(&g_failureCode, __ATOMIC_ACQUIRE) != 0)
        return;
    g_failureDetail = detail;
    __atomic_store_n(&g_failureCode, static_cast<int>(error), __ATOMIC_RELEASE);
}
static uint64_t g_latestDecodedReadyAtUs = 0;
static VideoPipelineTiming g_latestDecodedTiming;
static video::H264AnnexBParser g_h264Parser;
static video::VitaH264Decoder g_videoDecoder;
static std::vector<uint8_t> g_h264ParameterSets;
static bool g_decoderSawIdr = false;
static uint32_t g_decoderFailureLogs = 0;
// Arrival/input timestamps are IO-owned presentation cadence is main-owned.
static uint64_t g_lastMediaArrivalUs = 0;
static uint64_t g_lastMediaTimestampUs = 0;
static uint64_t g_lastPresentedAtUs = 0;
static uint64_t g_pendingInputSubmittedAtUs = 0;

constexpr int VIDEO_WORKER_PRIORITY = 0x10000100;
constexpr int VIDEO_WORKER_STACK_SIZE = 0x20000;
constexpr uint32_t VIDEO_QUEUE_WARNING_DEPTH = 8;
constexpr uint32_t VIDEO_QUEUE_WARNING_STEP = 8;

struct BasicH264AccessUnitKind {
    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;
};

void queueVideoAccessUnit(PendingVideoAccessUnit pending, uint32_t sequence);

BasicH264AccessUnitKind classifyH264AccessUnit(
    const uint8_t* data,
    size_t size)
{
    BasicH264AccessUnitKind kind{};

    for (size_t i = 0; i + 4 < size;)
    {
        size_t startCodeSize = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            startCodeSize = 3;
        else if (
            i + 5 <= size &&
            data[i] == 0 &&
            data[i + 1] == 0 &&
            data[i + 2] == 0 &&
            data[i + 3] == 1)
        {
            startCodeSize = 4;
        }

        if (startCodeSize == 0)
        {
            ++i;
            continue;
        }

        const size_t nalOffset = i + startCodeSize;
        if (nalOffset >= size)
            break;

        const uint8_t nalType = data[nalOffset] & 0x1f;
        if (nalType == 7)
            kind.hasSps = true;
        else if (nalType == 8)
            kind.hasPps = true;
        else if (nalType == 5)
            kind.hasIdr = true;

        i = nalOffset + 1;
    }

    return kind;
}

void acceptSessionMediaFrame(
    const android_auto::MediaFrame& frame,
    uint32_t& videoSequence,
    uint32_t& videoFrames,
    const char* logPrefix
)
{
    if (frame.channel != android_auto::DIRECT_VIDEO_CHANNEL ||
        frame.data == nullptr || frame.size == 0)
    {
        return;
    }

    const uint64_t arrivalUs = performance::nowUs();
    if (frame.rxFirstCompletedAtUs != 0 &&
        frame.parsedAtUs >= frame.rxFirstCompletedAtUs)
    {
        performance::record(performance::Metric::RxFirstToParsed,
            static_cast<uint32_t>(
                frame.parsedAtUs - frame.rxFirstCompletedAtUs
            )
        );
    }
    if (frame.rxLastCompletedAtUs != 0 &&
        frame.parsedAtUs >= frame.rxLastCompletedAtUs)
    {
        performance::record(performance::Metric::RxLastToParsed,
            static_cast<uint32_t>(
                frame.parsedAtUs - frame.rxLastCompletedAtUs
            )
        );
    }
    performance::frameArrived(arrivalUs, frame.timestamp, frame.size);
    if (g_lastMediaArrivalUs != 0)
        performance::recordCadence(performance::Cadence::ArrivalCadence, performance::elapsedUs(g_lastMediaArrivalUs));
    g_lastMediaArrivalUs = arrivalUs;

    if (frame.timestamp == 0) {
        performance::add(performance::Count::TimestampZero, 1);
    } else {
        if (g_lastMediaTimestampUs != 0) {
            if (frame.timestamp > g_lastMediaTimestampUs) {
                const uint64_t timestampDelta =
                    frame.timestamp - g_lastMediaTimestampUs;
                performance::recordCadence(performance::Cadence::AaCadence,
                    timestampDelta > UINT32_MAX
                        ? UINT32_MAX
                        : static_cast<uint32_t>(timestampDelta)
                );
            } else {
                performance::add(performance::Count::TimestampDiscontinuity, 1);
            }
        }
        g_lastMediaTimestampUs = frame.timestamp;
    }

    PendingVideoAccessUnit pending;
    pending.data.assign(frame.data, frame.data + frame.size);
    pending.timing.rxFirstCompletedAtUs = frame.rxFirstCompletedAtUs;
    pending.timing.rxLastCompletedAtUs = frame.rxLastCompletedAtUs;
    pending.timing.parsedAtUs = frame.parsedAtUs;
    const bool frameCouldFollowInput =
        frame.rxLastCompletedAtUs == 0 ||
        frame.rxLastCompletedAtUs >= g_pendingInputSubmittedAtUs;
    if (g_pendingInputSubmittedAtUs != 0 && frameCouldFollowInput &&
        frame.parsedAtUs >= g_pendingInputSubmittedAtUs)
    {
        pending.timing.inputSubmittedAtUs =
            g_pendingInputSubmittedAtUs;
        performance::record(performance::Metric::InputToMedia,
            static_cast<uint32_t>(
                frame.parsedAtUs - g_pendingInputSubmittedAtUs
            )
        );
        g_pendingInputSubmittedAtUs = 0;
    }
    queueVideoAccessUnit(std::move(pending), ++videoSequence);
    ++videoFrames;

    if (VITA_AUTO_VERBOSE_LOGGING &&
        (videoFrames == 1 || (videoFrames % 120) == 0)) {
        vitaLogInfo(
            "%s: video frames=%u latest=%u bytes",
            logPrefix,
            videoFrames,
            static_cast<uint32_t>(frame.size)
        );
    }
}

bool presentLatestDecodedFrameOnMainThread()
{

    const uint32_t availableSequence = __atomic_load_n(
        &g_decodedFrameSequence,
        __ATOMIC_ACQUIRE
    );
    // The released frame sequence also publishes the picture mutex created by IO.
    if (availableSequence == g_presentedFrameSequence || g_decodedFrameMutex < 0)
        return false;
    bool presented = false;

    // SceAvcdec owns the returned picture memory and may reuse it on the next
    // submit. Holding this mutex makes the GPU texture upload atomic with
    // respect to decoder output.
    const uint64_t lockStartUs = performance::nowUs();
    sceKernelLockMutex(g_decodedFrameMutex, 1, nullptr);
    performance::record(performance::Metric::PresentLockWait, performance::elapsedUs(lockStartUs));
    const uint32_t lockedSequence = __atomic_load_n(
        &g_decodedFrameSequence,
        __ATOMIC_ACQUIRE
    );
    if (lockedSequence != g_presentedFrameSequence && g_videoDecoder.hasFrame())
    {
        const auto& frame = g_videoDecoder.latestFrame();
        if (video::presentFrameWithGpu(frame) || video::presenter::presentCpuFallback(frame)) {
            presented = true;
            g_presentedFrameSequence = lockedSequence;
            __atomic_store_n(&g_hasPresentedFrame, 1u, __ATOMIC_RELEASE);
            performance::add(performance::Count::PresentedFrames, 1);

            const uint64_t presentedAtUs = performance::nowUs();
            if (g_latestDecodedTiming.rxFirstCompletedAtUs != 0 &&
                presentedAtUs >= g_latestDecodedTiming.rxFirstCompletedAtUs)
            {
                performance::record(performance::Metric::RxFirstToPresent,
                    static_cast<uint32_t>(
                        presentedAtUs -
                        g_latestDecodedTiming.rxFirstCompletedAtUs
                    )
                );
            }
            if (g_latestDecodedTiming.rxLastCompletedAtUs != 0 &&
                presentedAtUs >= g_latestDecodedTiming.rxLastCompletedAtUs)
            {
                performance::record(performance::Metric::RxLastToPresent,
                    static_cast<uint32_t>(
                        presentedAtUs -
                        g_latestDecodedTiming.rxLastCompletedAtUs
                    )
                );
            }
            if (g_latestDecodedTiming.parsedAtUs != 0 &&
                presentedAtUs >= g_latestDecodedTiming.parsedAtUs)
            {
                performance::record(performance::Metric::ParsedToPresent,
                    static_cast<uint32_t>(
                        presentedAtUs - g_latestDecodedTiming.parsedAtUs
                    )
                );
            }
            if (g_latestDecodedTiming.inputSubmittedAtUs != 0 &&
                presentedAtUs >= g_latestDecodedTiming.inputSubmittedAtUs)
            {
                performance::record(performance::Metric::InputToPresent,
                    static_cast<uint32_t>(
                        presentedAtUs -
                        g_latestDecodedTiming.inputSubmittedAtUs
                    )
                );
            }
            performance::framePresented(presentedAtUs);
            if (g_lastPresentedAtUs != 0)
                performance::recordCadence(performance::Cadence::PresentCadence,
                    performance::elapsedUs(g_lastPresentedAtUs)
                );
            g_lastPresentedAtUs = presentedAtUs;

            if (g_latestDecodedReadyAtUs != 0)
                performance::record(performance::Metric::ReadyWait, performance::elapsedUs(g_latestDecodedReadyAtUs));
            if (g_latestDecodedTiming.queuedAtUs != 0)
                performance::record(performance::Metric::EndToEnd, performance::elapsedUs(g_latestDecodedTiming.queuedAtUs));

            const video::GxmFrameTiming timing = video::lastGxmFrameTiming();
            performance::record(performance::Metric::GxmWait, timing.waitUs);
            performance::record(performance::Metric::GxmUpload, timing.uploadUs);
            performance::record(performance::Metric::GxmDraw, timing.drawUs);
            performance::record(performance::Metric::GxmTotal, timing.totalUs);
        }
    }
    sceKernelUnlockMutex(g_decodedFrameMutex, 1);
    return presented;
}

bool ensureVideoWorker();

void queueVideoAccessUnit(PendingVideoAccessUnit pending, uint32_t sequence)
{
    if (!ensureVideoWorker())
        return;

    const BasicH264AccessUnitKind kind =
        classifyH264AccessUnit(pending.data.data(), pending.data.size());

    sceKernelLockMutex(g_videoMutex, 1, nullptr);

    // H.264 access units must reach the decoder in order
    const uint32_t accessUnitBytes = static_cast<uint32_t>(pending.data.size());
    pending.timing.queuedAtUs = performance::nowUs();
    if (pending.timing.parsedAtUs != 0 &&
        pending.timing.queuedAtUs >= pending.timing.parsedAtUs)
    {
        performance::record(performance::Metric::ParsedToQueue,
            static_cast<uint32_t>(
                pending.timing.queuedAtUs -
                pending.timing.parsedAtUs
            )
        );
    }
    g_pendingVideoAccessUnits.push_back(std::move(pending));
    const uint32_t queueDepth = static_cast<uint32_t>(
        g_pendingVideoAccessUnits.size()
    );
    performance::recordQueueDepth(queueDepth);
    __atomic_store_n(
        &g_pendingVideoAccessUnitCount,
        queueDepth,
        __ATOMIC_RELEASE
    );
    const bool newPeak = queueDepth > g_peakPendingVideoAccessUnits;
    if (newPeak)
        g_peakPendingVideoAccessUnits = queueDepth;

    sceKernelUnlockMutex(g_videoMutex, 1);
    sceKernelSignalSema(g_videoReadySema, 1);

    const bool queueWarning = newPeak &&
        queueDepth >= VIDEO_QUEUE_WARNING_DEPTH &&
        (g_lastLoggedQueuePeak == 0 ||
         queueDepth >= g_lastLoggedQueuePeak + VIDEO_QUEUE_WARNING_STEP);
    if (queueWarning)
    {
        g_lastLoggedQueuePeak = queueDepth;
        vitaLog(
            "video queue backlog seq=%u bytes=%u depth=%u peak=%u params=%u idr=%u",
            sequence,
            accessUnitBytes,
            queueDepth,
            g_peakPendingVideoAccessUnits,
            (kind.hasSps || kind.hasPps) ? 1 : 0,
            kind.hasIdr ? 1 : 0
        );
    }

}

bool takeNextVideoAccessUnit(
    std::vector<uint8_t>& accessUnit,
    VideoPipelineTiming& pipelineTiming
)
{
    sceKernelLockMutex(g_videoMutex, 1, nullptr);

    if (g_pendingVideoAccessUnits.empty()) {
        sceKernelUnlockMutex(g_videoMutex, 1);
        return false;
    }

    PendingVideoAccessUnit& pending = g_pendingVideoAccessUnits.front();
    accessUnit.swap(pending.data);
    pipelineTiming = pending.timing;
    g_pendingVideoAccessUnits.pop_front();
    __atomic_store_n(
        &g_pendingVideoAccessUnitCount,
        static_cast<uint32_t>(g_pendingVideoAccessUnits.size()),
        __ATOMIC_RELEASE
    );
    sceKernelUnlockMutex(g_videoMutex, 1);
    return !accessUnit.empty();
}

void decodeVideoAccessUnit(
    std::vector<uint8_t>& accessUnit,
    const VideoPipelineTiming& pipelineTiming
)
{
    if (accessUnit.empty())
        return;

    const uint64_t decodeStartUs = performance::nowUs();
    if (pipelineTiming.queuedAtUs != 0)
        performance::record(performance::Metric::QueueWait, performance::elapsedUs(pipelineTiming.queuedAtUs));

    bool producedFrame = false;
    const video::H264AccessUnitInfo info =
        g_h264Parser.inspectAccessUnit(accessUnit.data(), accessUnit.size());
    if (info.parsedSps)
        vitaLogInfo("H264 SPS profile=%u poc=%u", info.profile, info.pictureOrderType);

    const uint64_t decoderLockStartUs = performance::nowUs();
    if (g_decodedFrameMutex >= 0)
        sceKernelLockMutex(g_decodedFrameMutex, 1, nullptr);
    performance::record(performance::Metric::DecodeLockWait, performance::elapsedUs(decoderLockStartUs));

    if (!g_videoDecoder.initialized() && info.parsedSps)
    {
        if (g_videoDecoder.init(info.width, info.height))
        {
            vitaLogInfo(
                "H264 decoder init ok size=%ux%u",
                info.width,
                info.height
            );
        }
        else
        {
            vitaLog(
            "[%s] H264 decoder init failed size=%ux%u stage=%u err=0x%08x", app::appErrorName(app::AppError::VideoDecoderInitFailed),
                info.width,
                info.height,
                g_videoDecoder.lastInitStage(),
                g_videoDecoder.lastError()
            );
            recordVideoFailure(app::AppError::VideoDecoderInitFailed, g_videoDecoder.lastError());
            sceKernelUnlockMutex(g_decodedFrameMutex, 1);
            return;
        }
    }

    if ((info.hasSps || info.hasPps) && !info.hasSlice)
    {
        g_h264ParameterSets = accessUnit;
        g_decoderSawIdr = false;
        if (!g_loggedVideoParameterSets) {
            vitaLogInfo(
                "H264 parameter sets saved bytes=%u nals=%u size=%ux%u",
                info.bytes,
                info.nalCount,
                info.width,
                info.height
            );
            g_loggedVideoParameterSets = true;
        }
    }

    if (g_videoDecoder.initialized() && info.hasSlice &&
        (g_decoderSawIdr || info.hasIdr))
    {
        const uint8_t* submitData = accessUnit.data();
        size_t submitSize = accessUnit.size();
        std::vector<uint8_t> combinedAccessUnit;

        if (info.hasIdr && !g_h264ParameterSets.empty())
        {
            combinedAccessUnit.reserve(g_h264ParameterSets.size() + accessUnit.size());
            combinedAccessUnit.insert(
                combinedAccessUnit.end(),
                g_h264ParameterSets.begin(),
                g_h264ParameterSets.end()
            );
            combinedAccessUnit.insert(
                combinedAccessUnit.end(),
                accessUnit.begin(),
                accessUnit.end()
            );
            submitData = combinedAccessUnit.data();
            submitSize = combinedAccessUnit.size();
            g_decoderSawIdr = true;
        }

        if (!g_videoDecoder.submitAccessUnit(submitData, submitSize))
        {
            ++g_decoderFailureLogs;
            if (g_decoderFailureLogs <= 8 || info.hasIdr)
            {
                vitaLog(
            "[%s] H264 decoder submit failed au=%u bytes=%u submit=%u idr=%u err=0x%08x", app::appErrorName(app::AppError::VideoDecodeFailed),
                    info.accessUnitCount,
                    info.bytes,
                    static_cast<uint32_t>(submitSize),
                    info.idrCount,
                    g_videoDecoder.lastError()
                );
            }
        }
        else
        {
            g_decoderFailureLogs = 0;
            producedFrame = g_videoDecoder.lastOutputCount() > 0;
            if (!g_loggedFirstDecodedFrame && producedFrame)
            {
                vitaLogInfo(
                    "H264 first decoded frame au=%u bytes=%u submit=%u submitted=%u idr=%u hasFrame=%u out=%u",
                    info.accessUnitCount,
                    info.bytes,
                    static_cast<uint32_t>(submitSize),
                    g_videoDecoder.submittedAccessUnits(),
                    info.idrCount,
                    g_videoDecoder.hasFrame() ? 1 : 0,
                    g_videoDecoder.lastOutputCount()
                );
                g_loggedFirstDecodedFrame = true;
            }
        }
    }

    if (producedFrame && g_videoDecoder.hasFrame())
    {
        g_latestDecodedTiming = pipelineTiming;
        g_latestDecodedReadyAtUs = performance::nowUs();
        __atomic_add_fetch(&g_decodedFrameSequence, 1, __ATOMIC_RELEASE);
    }

    if (g_decodedFrameMutex >= 0)
        sceKernelUnlockMutex(g_decodedFrameMutex, 1);

    performance::record(performance::Metric::Decode, performance::elapsedUs(decodeStartUs));

    // Presentation, including CPU fallback, belongs to the main thread.
}

int videoThreadEntry(SceSize, void*)
{
    vitaLogInfo("video thread entry");
    while (__atomic_load_n(&g_videoThreadRunning, __ATOMIC_ACQUIRE))
    {
        sceKernelWaitSema(g_videoReadySema, 1, nullptr);

        while (__atomic_load_n(&g_videoThreadRunning, __ATOMIC_ACQUIRE))
        {
            std::vector<uint8_t> accessUnit;
            VideoPipelineTiming pipelineTiming{};
            if (!takeNextVideoAccessUnit(accessUnit, pipelineTiming))
                break;

            decodeVideoAccessUnit(accessUnit, pipelineTiming);
            if (failure().error != app::AppError::None) {
                __atomic_store_n(&g_videoThreadRunning, false, __ATOMIC_RELEASE);
                break;
            }
            ++g_workerDecodedAccessUnits;
            if (g_workerDecodedAccessUnits == 1)
            {
                vitaLogInfo(
                    "video worker active count=%u saw_idr=%u has_frame=%u queued=%u",
                    g_workerDecodedAccessUnits,
                    g_decoderSawIdr ? 1 : 0,
                    g_videoDecoder.hasFrame() ? 1 : 0,
                    __atomic_load_n(&g_pendingVideoAccessUnitCount, __ATOMIC_ACQUIRE)
                );
            }
        }
    }

    return sceKernelExitThread(0);
}

// Called before the worker starts or after it has been joined.
void releaseVideoWorkerSyncObjects()
{
    if (g_videoReadySema >= 0) {
        sceKernelDeleteSema(g_videoReadySema);
        g_videoReadySema = -1;
    }
    if (g_decodedFrameMutex >= 0) {
        sceKernelDeleteMutex(g_decodedFrameMutex);
        g_decodedFrameMutex = -1;
    }
    if (g_videoMutex >= 0) {
        sceKernelDeleteMutex(g_videoMutex);
        g_videoMutex = -1;
    }
}

bool ensureVideoWorker()
{
    if (failure().error != app::AppError::None)
        return false;
    if (g_videoThread >= 0)
        return true;

    g_videoMutex = sceKernelCreateMutex("vita-auto-video", 0, 0, nullptr);
    if (g_videoMutex < 0) {
        recordVideoFailure(app::AppError::VideoQueueMutexCreateFailed, g_videoMutex);
        vitaLog(
            "[%s] video mutex create failed err=0x%08x", app::appErrorName(app::AppError::VideoQueueMutexCreateFailed), g_videoMutex);
        return false;
    }

    g_decodedFrameMutex = sceKernelCreateMutex(
        "vita-auto-decoded-frame",
        0,
        0,
        nullptr
    );
    if (g_decodedFrameMutex < 0) {
        recordVideoFailure(app::AppError::VideoPictureMutexCreateFailed, g_decodedFrameMutex);
        vitaLog(
            "[%s] decoded frame mutex create failed err=0x%08x", app::appErrorName(app::AppError::VideoPictureMutexCreateFailed), g_decodedFrameMutex);
        releaseVideoWorkerSyncObjects();
        return false;
    }

    g_videoReadySema = sceKernelCreateSema("vita-auto-video-ready", 0, 0, 1, nullptr);
    if (g_videoReadySema < 0) {
        recordVideoFailure(app::AppError::VideoSemaphoreCreateFailed, g_videoReadySema);
        vitaLog(
            "[%s] video sema create failed err=0x%08x", app::appErrorName(app::AppError::VideoSemaphoreCreateFailed), g_videoReadySema);
        releaseVideoWorkerSyncObjects();
        return false;
    }

    g_videoThread = sceKernelCreateThread(
        "vita-auto-video",
        videoThreadEntry,
        VIDEO_WORKER_PRIORITY,
        VIDEO_WORKER_STACK_SIZE,
        0,
        0,
        nullptr
    );
    if (g_videoThread < 0) {
        recordVideoFailure(app::AppError::VideoThreadCreateFailed, g_videoThread);
        vitaLog(
            "[%s] video thread create failed err=0x%08x", app::appErrorName(app::AppError::VideoThreadCreateFailed), g_videoThread);
        releaseVideoWorkerSyncObjects();
        return false;
    }

    __atomic_store_n(&g_videoThreadRunning, true, __ATOMIC_RELEASE);
    const int startResult = sceKernelStartThread(g_videoThread, 0, nullptr);
    if (startResult < 0) {
        recordVideoFailure(app::AppError::VideoThreadStartFailed, startResult);
        vitaLog(
            "[%s] video thread start failed err=0x%08x", app::appErrorName(app::AppError::VideoThreadStartFailed), startResult);
        __atomic_store_n(&g_videoThreadRunning, false, __ATOMIC_RELEASE);
        sceKernelDeleteThread(g_videoThread);
        g_videoThread = -1;
        releaseVideoWorkerSyncObjects();
        return false;
    }

    vitaLogInfo("video thread started");
    return true;
}

void stopVideoWorker()
{
    if (g_videoThread >= 0)
    {
        __atomic_store_n(&g_videoThreadRunning, false, __ATOMIC_RELEASE);
        if (g_videoReadySema >= 0)
            sceKernelSignalSema(g_videoReadySema, 1);
        sceKernelWaitThreadEnd(g_videoThread, nullptr, nullptr);
        sceKernelDeleteThread(g_videoThread);
        g_videoThread = -1;
    }

    releaseVideoWorkerSyncObjects();

    __atomic_store_n(&g_decodedFrameSequence, 0, __ATOMIC_RELEASE);
    g_presentedFrameSequence = 0;
    __atomic_store_n(&g_hasPresentedFrame, 0u, __ATOMIC_RELEASE);
    g_failureDetail = 0;
    __atomic_store_n(&g_failureCode, 0, __ATOMIC_RELEASE);

    g_pendingVideoAccessUnits.clear();
    g_latestDecodedTiming = VideoPipelineTiming{};
    g_pendingInputSubmittedAtUs = 0;
    __atomic_store_n(&g_pendingVideoAccessUnitCount, 0, __ATOMIC_RELEASE);
    g_peakPendingVideoAccessUnits = 0;
    g_lastLoggedQueuePeak = 0;
    g_loggedVideoParameterSets = false;
    g_loggedFirstDecodedFrame = false;
    g_h264ParameterSets.clear();
    g_h264Parser = video::H264AnnexBParser{};
    g_workerDecodedAccessUnits = 0;
    g_decoderFailureLogs = 0;
    g_latestDecodedReadyAtUs = 0;
    g_lastMediaArrivalUs = 0;
    g_lastMediaTimestampUs = 0;
    g_lastPresentedAtUs = 0;
    g_decoderSawIdr = false;
    vitaLogInfo("H264 decoder totals submitted=%u output=%u",
        g_videoDecoder.submittedAccessUnits(), g_videoDecoder.outputPictures());
    g_videoDecoder.shutdown();
}

} // namespace

void acceptFrame(const android_auto::MediaFrame& frame,
                 uint32_t& sequence, uint32_t& frames, const char* logPrefix)
{
    acceptSessionMediaFrame(frame, sequence, frames, logPrefix);
}

void noteInputSubmitted(uint64_t submittedAtUs)
{
    g_pendingInputSubmittedAtUs = submittedAtUs;
}

Failure failure()
{
    const auto error = static_cast<app::AppError>(
        __atomic_load_n(&g_failureCode, __ATOMIC_ACQUIRE));
    return {error, error == app::AppError::None ? 0 : g_failureDetail};
}

bool hasPresentedFrame()
{
    return __atomic_load_n(&g_hasPresentedFrame, __ATOMIC_ACQUIRE) != 0;
}

uint32_t peakQueueDepth() { return g_peakPendingVideoAccessUnits; }
bool present() { return presentLatestDecodedFrameOnMainThread(); }
void stop() { stopVideoWorker(); }

} // namespace pipeline
} // namespace video
