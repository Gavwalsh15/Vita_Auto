#include "android_auto/aa_direct.h"

#include <psp2/kernel/threadmgr.h>

#include "android_auto/aa_direct_messages.h"
#include "logger/performance.h"

namespace android_auto {
namespace {

constexpr uint8_t ENCRYPTED_SIGNAL_FLAGS = 0x0b;
constexpr uint16_t AV_MEDIA_WITH_TIMESTAMP = 0x0000;
constexpr uint16_t AV_MEDIA = 0x0001;
constexpr uint16_t AV_MEDIA_ACK = 0x8004;
constexpr size_t MEDIA_TIMESTAMP_SIZE = 8;

} // namespace

bool DirectSession::sendMediaAck(uint8_t channel, uint32_t count)
{
    // ack_count must be > 0 or the phone ignores the acknowledgement.
    if (channel >= 16 || count == 0)
        return false;

    FixedWriter<16> ack;
    const bool queued = ack.varintField(
        1,
        mediaSessions_[channel]
    ) && ack.varintField(2, count) && sendEncryptedRaw(
        channel,
        ENCRYPTED_SIGNAL_FLAGS,
        AV_MEDIA_ACK,
        ack.data(),
        ack.size()
    );

#if VITA_AUTO_PERFORMANCE
    if (queued) {
        const uint64_t nowUs = sceKernelGetSystemTimeWide();
        if (mediaAcksPending_ == 0)
            mediaAckOldestPendingUs_ = nowUs;
        ++mediaAcksQueued_;
        ++mediaAcksPending_;
    }
#endif
    return queued;
}

bool DirectSession::processMedia(
    uint8_t channel,
    uint16_t messageId,
    const uint8_t* payload,
    size_t payloadSize
) {
    if (messageId != AV_MEDIA && messageId != AV_MEDIA_WITH_TIMESTAMP)
        return true;

    uint64_t timestamp = 0;
    size_t offset = 0;
    if (messageId == AV_MEDIA_WITH_TIMESTAMP &&
        payloadSize >= MEDIA_TIMESTAMP_SIZE)
    {
        for (size_t i = 0; i < MEDIA_TIMESTAMP_SIZE; ++i)
            timestamp = (timestamp << 8) | payload[i];
        offset = MEDIA_TIMESTAMP_SIZE;
    }

    if (mediaHandler_ && payload != nullptr && payloadSize >= offset) {
        MediaFrame frame;
        frame.channel = channel;
        frame.timestamp = timestamp;
        frame.rxFirstCompletedAtUs = messageRxFirstCompletedAtUs_;
        frame.rxLastCompletedAtUs = messageRxLastCompletedAtUs_;
        frame.parsedAtUs = logger::performance::nowUs();
        frame.data = payload + offset;
        frame.size = payloadSize - offset;
        mediaHandler_(frame);
    }

    return sendMediaAck(channel, 1);
}

} // namespace android_auto
