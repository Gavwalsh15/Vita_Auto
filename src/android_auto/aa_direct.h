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
    VersionNegotiating = 1, // Wi-Fi only: the transport did not do it for us.
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
    PointerDown = 5,
    PointerUp = 6,
};

constexpr size_t MAX_TOUCH_POINTERS = 6;

struct TouchPointer {
    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t id = 0;
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
    // For transports that leave version negotiation to us (Wi-Fi): send
    // VERSION_REQUEST 1.7 and begin TLS once the phone accepts it.
    bool start();
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

    // pointers lists every finger currently down. actionIndex says which one a
    // PointerDown or PointerUp is about; it is 0 for the other actions.
    bool sendTouch(
        TouchAction action,
        uint8_t actionIndex,
        const TouchPointer* pointers,
        size_t count
    );
    bool sendButton(uint32_t keycode, bool down);
    bool sendRotary(int32_t delta);
    bool sendShutdownRequest();
    bool sendPingRequest();
    // Unsolicited video focus: projected true gains it (asks the phone for video),
    // false releases it to NATIVE. Does nothing until the video channel is set up.
    bool sendVideoFocus(bool projected = true);
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
    // Media frames the phone may send before waiting for an ACK. Set before
    // start(); it is advertised in each AV setup response.
    void setMediaMaxUnacked(uint32_t maxUnacked) { mediaMaxUnacked_ = maxUnacked > 0 ? maxUnacked : 1; }

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
    bool beginTls();
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
    uint32_t mediaMaxUnacked_ = 1;
    bool microphoneOpen_ = false;
    // Supplied by the phone's microphone request, independently of our sink limit.
    uint32_t microphoneMaxUnacked_ = 1;
    uint32_t microphoneFramesInFlight_ = 0;
    uint32_t microphoneFramesSent_ = 0;
    uint32_t microphoneAcksReceived_ = 0;
    std::array<DecryptedFragment, 256> decryptedFragments_{};
    MediaHandler mediaHandler_;
    uint64_t pendingTlsRxFirstCompletedAtUs_ = 0;
    uint64_t pendingTlsRxLastCompletedAtUs_ = 0;
    std::vector<uint8_t> tlsReadScratch_;
    std::vector<uint8_t> tlsPlainScratch_;
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
