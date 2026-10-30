#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "android_auto/aa_protocol.h"
#include "android_auto/aa_errors.h"
#include "android_auto/aa_tls.h"

namespace android_auto {

constexpr uint8_t DIRECT_INPUT_CHANNEL = 1;
constexpr uint8_t DIRECT_SENSOR_CHANNEL = 2;
constexpr uint8_t DIRECT_VIDEO_CHANNEL = 3;
constexpr uint8_t DIRECT_MEDIA_AUDIO_CHANNEL = 4;
constexpr uint8_t DIRECT_GUIDANCE_AUDIO_CHANNEL = 5;
constexpr uint8_t DIRECT_SYSTEM_AUDIO_CHANNEL = 6;
constexpr uint8_t DIRECT_MICROPHONE_CHANNEL = 7;

enum class DirectState {
    Stopped,
    TlsNegotiating = 3, // Preserve the existing state numbers in logs.
    TlsOk,
    ServiceReady,
    ChannelsReady,
    MediaReady,
    Error,
};

enum class TouchAction : uint8_t {
    Down = 0,
    Up = 1,
    Move = 2,
};

struct MediaFrame {
    uint8_t channel = 0;
    uint64_t timestamp = 0;
    uint64_t rxFirstCompletedAtUs = 0;
    uint64_t rxLastCompletedAtUs = 0;
    uint64_t parsedAtUs = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct MediaAckStats {
    uint32_t queued = 0;
    uint32_t submitted = 0;
    uint32_t pending = 0;
    uint32_t submitSamples = 0;
    uint64_t submitLatencyTotalUs = 0;
    uint32_t submitLatencyMaxUs = 0;
};

class DirectSession {
public:
    using MediaHandler = std::function<void(const MediaFrame&)>;

    explicit DirectSession(bool audioOnVita);
    ~DirectSession() = default;

    DirectSession(const DirectSession&) = delete;
    DirectSession& operator=(const DirectSession&) = delete;

    void reset();
    // The native USB kernel transport has already completed AA version 1.7.
    // Begin directly at the TLS stage without sending VERSION_REQUEST twice.
    bool startAfterVersion();
    bool feed(
        const uint8_t* data,
        size_t length,
        uint64_t rxFirstCompletedAtUs = 0,
        uint64_t rxLastCompletedAtUs = 0
    );
    const uint8_t* outboundData() const { return outbound_.data(); }
    size_t outboundSize() const { return outbound_.size(); }
    void clearOutbound() { outbound_.clear(); }

    // Called only after the transport has accepted the complete outbound
    // batch. This distinguishes an ACK merely encrypted/queued in userspace
    // from one actually submitted to the kernel USB writer.
    void markOutboundSubmitted(uint64_t submittedAtUs);
    MediaAckStats mediaAckStats() const;

    bool sendTouch(TouchAction action, uint16_t x, uint16_t y, uint32_t pointerId = 0);
    bool sendButton(uint32_t keycode, bool down);
    bool sendRotary(int32_t delta);
    bool sendShutdownRequest();
    bool sendPingRequest();
    bool sendMicrophone(
        const int16_t* samples,
        size_t sampleCount,
        uint64_t timestampUs
    );

    bool microphoneOpen() const { return microphoneOpen_; }
    bool microphoneCanSend() const
    {
        return microphoneOpen_ && microphoneFramesInFlight_ < microphoneMaxUnacked_;
    }
    uint32_t microphoneFramesSent() const { return microphoneFramesSent_; }
    uint32_t microphoneAcksReceived() const { return microphoneAcksReceived_; }

    void setMediaHandler(MediaHandler handler);

    DirectState state() const { return state_; }
    int lastError() const { return static_cast<int>(lastError_); }
    const char* lastErrorName() const { return directErrorName(lastError_); }
    int lastTlsError() const { return lastTlsError_; }
    bool inputReady() const;

private:
    bool fail(DirectError error, int tlsError = 0);
    struct DecryptedFragment {
        bool active = false;
        uint8_t messageBits = 0;
        uint64_t rxFirstCompletedAtUs = 0;
        uint64_t rxLastCompletedAtUs = 0;
        std::vector<uint8_t> payload;
    };

    bool queueFrame(std::vector<uint8_t> frame);
    bool flushTlsOutbound(uint8_t channel, uint8_t flags);
    bool sendPlainControl(uint16_t messageId, const std::vector<uint8_t>& payload);
    bool sendEncrypted(
        uint8_t channel,
        uint8_t flags,
        uint16_t messageId,
        const std::vector<uint8_t>& payload
    );
    bool sendEncryptedRaw(
        uint8_t channel,
        uint8_t flags,
        uint16_t messageId,
        const uint8_t* payload,
        size_t payloadSize
    );
    bool processFrame(const Frame& frame);
    bool processFrames(const std::vector<Frame>& frames);
    void beginResync(
        const char* reason,
        uint64_t rxFirstCompletedAtUs,
        uint64_t rxLastCompletedAtUs
    );
    bool continueResync();
    enum class ResyncScanResult {
        Found,
        NeedMore,
        NotFound,
    };
    ResyncScanResult scanResync(
        size_t& offset,
        size_t& frameLength,
        uint64_t& sequence
    ) const;
    bool sequencePlausible(uint64_t sequence) const;
    bool processDecryptedFrame(
        uint8_t channel,
        uint8_t flags,
        const std::vector<uint8_t>& plain,
        uint64_t rxFirstCompletedAtUs,
        uint64_t rxLastCompletedAtUs
    );
    bool processControlMessage(uint16_t messageId, const std::vector<uint8_t>& payload);
    bool processDecryptedMessage(
        uint8_t channel,
        uint8_t flags,
        const uint8_t* data,
        size_t length,
        uint64_t rxFirstCompletedAtUs,
        uint64_t rxLastCompletedAtUs
    );
    bool processMedia(
        uint8_t channel,
        uint16_t messageId,
        const uint8_t* payload,
        size_t payloadSize
    );

    const bool audioOnVita_;
    DirectState state_ = DirectState::Stopped;
    DirectError lastError_ = DirectError::None;
    int lastTlsError_ = 0;
    FrameParser parser_;
    std::unique_ptr<TlsClient> tls_;
    std::vector<uint8_t> outbound_;
    uint32_t openedChannels_ = 0;
    uint64_t messageRxFirstCompletedAtUs_ = 0;
    uint64_t messageRxLastCompletedAtUs_ = 0;
    uint32_t mediaSessions_[16] = {};
    bool microphoneOpen_ = false;
    // Supplied by the phone's microphone request, independently of our sink limit.
    uint32_t microphoneMaxUnacked_ = 1;
    uint32_t microphoneFramesInFlight_ = 0;
    uint32_t microphoneFramesSent_ = 0;
    uint32_t microphoneAcksReceived_ = 0;
    std::array<DecryptedFragment, 256> decryptedFragments_{};
    MediaHandler mediaHandler_;
    bool inboundTlsSequenceKnown_ = false;
    uint64_t inboundTlsLastSequence_ = 0;
    uint32_t inboundTlsRecordCount_ = 0;
    uint32_t inboundTlsSequenceAnomalies_ = 0;
    uint64_t pendingTlsRxFirstCompletedAtUs_ = 0;
    uint64_t pendingTlsRxLastCompletedAtUs_ = 0;
    std::vector<uint8_t> tlsReadScratch_;
    std::vector<uint8_t> tlsPlainScratch_;
    // AA-frame resync after a lost USB chunk breaks TLS record alignment.
    bool resyncing_ = false;
    bool tlsAwaitingSequence_ = false;
    std::vector<uint8_t> resyncBuffer_;
    uint64_t resyncRxFirstCompletedAtUs_ = 0;
    uint64_t resyncRxLastCompletedAtUs_ = 0;
    uint32_t resyncEvents_ = 0;
    uint32_t resyncSkippedFrames_ = 0;
#if VITA_AUTO_PERFORMANCE
    uint32_t mediaAcksQueued_ = 0;
    uint32_t mediaAcksSubmitted_ = 0;
    uint32_t mediaAcksPending_ = 0;
    uint32_t mediaAckSubmitSamples_ = 0;
    uint64_t mediaAckOldestPendingUs_ = 0;
    uint64_t mediaAckSubmitLatencyTotalUs_ = 0;
    uint32_t mediaAckSubmitLatencyMaxUs_ = 0;
#endif
};

} // namespace android_auto
