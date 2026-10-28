#include "android_auto/aa_direct.h"

#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "android_auto/aa_direct_messages.h"
#include "android_auto/aa_service_discovery.h"
#include "logger/vita_log.h"

namespace android_auto {

namespace {

constexpr uint8_t ENCRYPTED_SIGNAL_FLAGS = 0x0b;
constexpr uint8_t ENCRYPTED_CONTROL_FLAGS = 0x0f;
constexpr uint8_t FRAME_TYPE_MASK = 0x03;
constexpr uint8_t FRAME_FIRST = 0x01;
constexpr uint8_t FRAME_LAST = 0x02;
constexpr uint8_t FRAME_BULK = FRAME_FIRST | FRAME_LAST;
constexpr size_t TLS_READ_CHUNK_SIZE = 16 * 1024;
constexpr uint32_t MICROPHONE_SESSION_ID = 1; // First microphone stream session.

constexpr uint16_t AUTH_COMPLETE = 0x0004;
constexpr uint16_t SERVICE_DISCOVERY_REQUEST = 0x0005;
constexpr uint16_t SERVICE_DISCOVERY_RESPONSE = 0x0006;
constexpr uint16_t CHANNEL_OPEN_REQUEST = 0x0007;
constexpr uint16_t CHANNEL_OPEN_RESPONSE = 0x0008;
constexpr uint16_t PING_REQUEST = 0x000b;
constexpr uint16_t PING_RESPONSE = 0x000c;
constexpr uint16_t NAV_FOCUS_REQUEST = 0x000d;
constexpr uint16_t NAV_FOCUS_RESPONSE = 0x000e;
constexpr uint16_t SHUTDOWN_RESPONSE = 0x0010;
constexpr uint16_t AUDIO_FOCUS_REQUEST = 0x0012;
constexpr uint16_t AUDIO_FOCUS_RESPONSE = 0x0013;
constexpr uint16_t AV_SETUP_REQUEST = 0x8000;
constexpr uint16_t AV_START_INDICATION = 0x8001;
constexpr uint16_t AV_STOP_INDICATION = 0x8002;
constexpr uint16_t AV_SETUP_RESPONSE = 0x8003;
constexpr uint16_t AV_MEDIA_ACK = 0x8004;
constexpr uint16_t MICROPHONE_REQUEST = 0x8005;
constexpr uint16_t MICROPHONE_RESPONSE = 0x8006;
constexpr uint16_t VIDEO_FOCUS_REQUEST = 0x8007;
constexpr uint16_t VIDEO_FOCUS_INDICATION = 0x8008;
constexpr uint16_t INPUT_BINDING_REQUEST = 0x8002;
constexpr uint16_t INPUT_BINDING_RESPONSE = 0x8003;
constexpr uint16_t SENSOR_START_REQUEST = 0x8001;
constexpr uint16_t SENSOR_START_RESPONSE = 0x8002;
constexpr uint16_t SENSOR_EVENT = 0x8003;

} // namespace

DirectSession::DirectSession(bool audioOnVita) : audioOnVita_(audioOnVita)
{
    reset();
}

void DirectSession::reset()
{
    state_ = DirectState::Stopped;
    lastError_ = DirectError::None;
    lastTlsError_ = 0;
    parser_ = FrameParser();
    tls_.reset();
    outbound_.clear();
    if (outbound_.capacity() < 64 * 1024)
        outbound_.reserve(64 * 1024);
    openedChannels_ = 0;
    std::memset(mediaSessions_, 0, sizeof(mediaSessions_));
    decryptedFragments_ = {};
    inboundTlsSequenceKnown_ = false;
    inboundTlsLastSequence_ = 0;
    inboundTlsRecordCount_ = 0;
    inboundTlsSequenceAnomalies_ = 0;
    pendingTlsRxFirstCompletedAtUs_ = 0;
    pendingTlsRxLastCompletedAtUs_ = 0;
    tlsReadScratch_.clear();
    tlsPlainScratch_.clear();
#if VITA_AUTO_PERFORMANCE
    mediaAcksQueued_ = 0;
    mediaAcksSubmitted_ = 0;
    mediaAcksPending_ = 0;
    mediaAckSubmitSamples_ = 0;
    mediaAckOldestPendingUs_ = 0;
    mediaAckSubmitLatencyTotalUs_ = 0;
    mediaAckSubmitLatencyMaxUs_ = 0;
#endif
    microphoneOpen_ = false;
    microphoneMaxUnacked_ = 1; // Safe fallback until the phone advertises a limit.
    microphoneFramesInFlight_ = 0;
    microphoneFramesSent_ = 0;
    microphoneAcksReceived_ = 0;
}

bool DirectSession::fail(DirectError error, int tlsError)
{
    if (lastError_ == DirectError::None) {
        lastError_ = error;
        lastTlsError_ = tlsError;
    }
    state_ = DirectState::Error;
    return false;
}

bool DirectSession::startAfterVersion()
{
    reset();
    return beginTls();
}

bool DirectSession::start()
{
    reset();
    state_ = DirectState::VersionNegotiating;
    return sendPlainControl(VERSION_REQUEST, {0x00, 0x01, 0x00, 0x07});
}

bool DirectSession::beginTls()
{
    tls_.reset(new TlsClient());
    if (!tls_ || !tls_->start()) {
        fail(DirectError::TlsStartFailed, tls_ ? tls_->lastError() : 0);
        vitaLog(
            "direct-usb: TLS start failed err=%d (%s) tls=0x%08x",
            lastError(),
            lastErrorName(),
            lastTlsError_
        );
        return false;
    }

    state_ = DirectState::TlsNegotiating;
    return flushTlsOutbound(CONTROL_CHANNEL, ENCRYPTED_SIGNAL_FLAGS);
}

bool DirectSession::queueFrame(std::vector<uint8_t> frame)
{
    if (frame.empty()) {
        return fail(DirectError::InvalidOutboundFrame);
    }
    outbound_.insert(outbound_.end(), frame.begin(), frame.end());
    return true;
}

void DirectSession::markOutboundSubmitted(uint64_t submittedAtUs)
{
#if VITA_AUTO_PERFORMANCE
    if (mediaAcksPending_ == 0)
        return;

    uint64_t latencyUs = 0;
    if (mediaAckOldestPendingUs_ != 0 &&
        submittedAtUs >= mediaAckOldestPendingUs_)
    {
        latencyUs = submittedAtUs - mediaAckOldestPendingUs_;
    }
    const uint32_t boundedLatencyUs = latencyUs > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(latencyUs);

    mediaAcksSubmitted_ += mediaAcksPending_;
    ++mediaAckSubmitSamples_;
    mediaAckSubmitLatencyTotalUs_ += boundedLatencyUs;
    if (boundedLatencyUs > mediaAckSubmitLatencyMaxUs_)
        mediaAckSubmitLatencyMaxUs_ = boundedLatencyUs;

    mediaAcksPending_ = 0;
    mediaAckOldestPendingUs_ = 0;
#else
    (void)submittedAtUs;
#endif
}

MediaAckStats DirectSession::mediaAckStats() const
{
    MediaAckStats stats{};
#if VITA_AUTO_PERFORMANCE
    stats.queued = mediaAcksQueued_;
    stats.submitted = mediaAcksSubmitted_;
    stats.pending = mediaAcksPending_;
    stats.submitSamples = mediaAckSubmitSamples_;
    stats.submitLatencyTotalUs = mediaAckSubmitLatencyTotalUs_;
    stats.submitLatencyMaxUs = mediaAckSubmitLatencyMaxUs_;
#endif
    return stats;
}

bool DirectSession::flushTlsOutbound(uint8_t channel, uint8_t flags)
{
    if (!tls_)
        return false;

    const size_t tlsSize = tls_->outboundSize();
    if (tlsSize == 0)
        return true;

    const bool handshakeBytes = tls_->outboundIsHandshake();
    const size_t bodySize = tlsSize + (handshakeBytes ? 2u : 0u);
    if (bodySize > UINT16_MAX) {
        return fail(DirectError::InvalidOutboundFrame);
    }

    outbound_.reserve(outbound_.size() + HEADER_SIZE + bodySize);
    outbound_.push_back(handshakeBytes ? CONTROL_CHANNEL : channel);
    outbound_.push_back(
        handshakeBytes
            ? PLAINTEXT_CONTROL_FLAGS
            : static_cast<uint8_t>(flags | ENCRYPTED)
    );
    writeBe16(outbound_, static_cast<uint16_t>(bodySize));
    if (handshakeBytes)
        writeBe16(outbound_, SSL_HANDSHAKE);
    outbound_.insert(
        outbound_.end(),
        tls_->outboundData(),
        tls_->outboundData() + tlsSize
    );
    tls_->clearOutbound();
    return true;
}

bool DirectSession::sendPlainControl(
    uint16_t messageId,
    const std::vector<uint8_t>& payload
) {
    return queueFrame(encodeFrame(
        CONTROL_CHANNEL,
        PLAINTEXT_CONTROL_FLAGS,
        messageId,
        payload
    ));
}

bool DirectSession::sendEncrypted(
    uint8_t channel,
    uint8_t flags,
    uint16_t messageId,
    const std::vector<uint8_t>& payload
) {
    return sendEncryptedRaw(
        channel,
        flags,
        messageId,
        payload.data(),
        payload.size()
    );
}

bool DirectSession::sendEncryptedRaw(
    uint8_t channel,
    uint8_t flags,
    uint16_t messageId,
    const uint8_t* payload,
    size_t payloadSize
) {
    if (state_ == DirectState::Error)
        return false;
    if (!tls_ || !tls_->secure()) {
        return fail(DirectError::EncryptedSendRejected);
    }

    constexpr size_t MAX_OUTBOUND_PAYLOAD = 2046;
    if ((payload == nullptr && payloadSize != 0) ||
        payloadSize > MAX_OUTBOUND_PAYLOAD)
    {
        return fail(DirectError::EncryptedSendRejected);
    }
    uint8_t plain[MAX_OUTBOUND_PAYLOAD + 2];
    plain[0] = static_cast<uint8_t>((messageId >> 8) & 0xff);
    plain[1] = static_cast<uint8_t>(messageId & 0xff);
    if (payloadSize != 0)
        std::memcpy(plain + 2, payload, payloadSize);

    if (!tls_->write(plain, payloadSize + 2)) {
        return fail(DirectError::TlsWriteFailed, tls_->lastError());
    }

    return flushTlsOutbound(channel, flags);
}

bool DirectSession::feed(
    const uint8_t* data,
    size_t length,
    uint64_t rxFirstCompletedAtUs,
    uint64_t rxLastCompletedAtUs
)
{
    if (state_ == DirectState::Error)
        return false;
    if (data == nullptr && length != 0)
        return fail(DirectError::InvalidInput);

    const std::vector<Frame> frames = parser_.push(
        data,
        length,
        rxFirstCompletedAtUs,
        rxLastCompletedAtUs
    );
    if (!std::all_of(frames.begin(), frames.end(),
            [this](const Frame& frame) { return processFrame(frame); }))
        return false;

    if (parser_.failed()) {
        const auto& head = parser_.failureHead();
        vitaLog(
            "AA parser failed code=%d (%s) buffered=%u rx=%u head=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            parser_.errorCode(),
            parser_.errorName(),
            static_cast<unsigned int>(parser_.failureBufferedSize()),
            static_cast<unsigned int>(length),
            head[0], head[1], head[2], head[3],
            head[4], head[5], head[6], head[7],
            head[8], head[9], head[10], head[11],
            head[12], head[13], head[14], head[15]
        );
        return fail(DirectError::FrameParseFailed);
    }

    return true;
}

bool DirectSession::processFrame(const Frame& frame)
{
    if (!frame.encrypted) {
        if (frame.channel == CONTROL_CHANNEL &&
            frame.flags == PLAINTEXT_CONTROL_FLAGS &&
            frame.messageId == VERSION_RESPONSE &&
            state_ == DirectState::VersionNegotiating)
        {
            if (frame.payload.size() < 6 ||
                readBe16(frame.payload.data() + 4) != VERSION_STATUS_MATCH)
            {
                vitaLog("direct-usb: phone rejected AA version request");
                return fail(DirectError::VersionNegotiationFailed);
            }
            return beginTls();
        }

        if (frame.channel == CONTROL_CHANNEL &&
            frame.flags == PLAINTEXT_CONTROL_FLAGS &&
            frame.messageId == VERSION_RESPONSE)
        {
            vitaLog("direct-usb: unexpected VERSION_RESPONSE after kernel version handshake");
            return fail(DirectError::VersionNegotiationFailed);
        }

        if (frame.channel == CONTROL_CHANNEL &&
            frame.messageId == SSL_HANDSHAKE &&
            tls_)
        {
            if (!tls_->feed(frame.payload.data(), frame.payload.size())) {
                return fail(DirectError::TlsHandshakeFeedFailed, tls_->lastError());
            }

            if (!flushTlsOutbound(CONTROL_CHANNEL, ENCRYPTED_SIGNAL_FLAGS))
                return false;

            if (tls_->secure() && state_ != DirectState::TlsOk) {
                state_ = DirectState::TlsOk;
                std::vector<uint8_t> auth;
                writeVarintField(auth, 1, 0); // Field 1: auth status OK.
                // AUTH Complete
                return sendPlainControl(AUTH_COMPLETE, auth);
            }
            return true;
        }

        if (frame.channel == CONTROL_CHANNEL &&
            frame.messageId == PING_REQUEST)
        {
            return sendPlainControl(PING_RESPONSE, frame.payload);
        }
        return true;
    }

    const TlsRecordInspection inspection = inspectTlsRecords(frame.payload);
    if (frame.rxFirstCompletedAtUs != 0 &&
        (pendingTlsRxFirstCompletedAtUs_ == 0 ||
         frame.rxFirstCompletedAtUs < pendingTlsRxFirstCompletedAtUs_))
    {
        pendingTlsRxFirstCompletedAtUs_ = frame.rxFirstCompletedAtUs;
    }
    if (frame.rxLastCompletedAtUs > pendingTlsRxLastCompletedAtUs_)
        pendingTlsRxLastCompletedAtUs_ = frame.rxLastCompletedAtUs;
    const uint64_t expectedSequence = inboundTlsLastSequence_ + 1;
    const bool sequenceMatches =
        !inboundTlsSequenceKnown_ ||
        (inspection.layoutValid && inspection.hasSequence &&
         inspection.firstSequence == expectedSequence &&
         inspection.internallyContiguous);

    if (!tls_ || !tls_->feed(frame.payload.data(), frame.payload.size())) {
        fail(DirectError::TlsRecordFeedFailed, tls_ ? tls_->lastError() : 0);
        vitaLog(
            "direct-usb: TLS feed error=0x%08x records=%u layout=%u seq=%08x%08x expected=%08x%08x match=%u hash=%08x cipher=%u",
            lastTlsError_,
            inspection.recordCount,
            inspection.layoutValid ? 1 : 0,
            static_cast<uint32_t>(inspection.firstSequence >> 32),
            static_cast<uint32_t>(inspection.firstSequence),
            static_cast<uint32_t>(expectedSequence >> 32),
            static_cast<uint32_t>(expectedSequence),
            sequenceMatches ? 1 : 0,
            tlsPayloadHash(frame.payload),
            static_cast<uint32_t>(frame.payload.size())
        );
        return false;
    }

    if (tlsReadScratch_.size() != TLS_READ_CHUNK_SIZE)
        tlsReadScratch_.resize(TLS_READ_CHUNK_SIZE);
    tlsPlainScratch_.clear();
    if (tlsPlainScratch_.capacity() < frame.payload.size())
        tlsPlainScratch_.reserve(frame.payload.size());
    for (;;) {
        const int read = tls_->read(tlsReadScratch_.data(), tlsReadScratch_.size());
        if (read < 0) {
            fail(DirectError::TlsReadFailed, tls_->lastError());
            vitaLog(
                "direct-usb: TLS read error=0x%08x channel=%u flags=0x%02x cipher=%u records=%u layout=%u seq=%08x%08x expected=%08x%08x match=%u hash=%08x accepted=%u anomalies=%u",
                lastTlsError_,
                frame.channel,
                frame.flags,
                static_cast<uint32_t>(frame.payload.size()),
                inspection.recordCount,
                inspection.layoutValid ? 1 : 0,
                static_cast<uint32_t>(inspection.firstSequence >> 32),
                static_cast<uint32_t>(inspection.firstSequence),
                static_cast<uint32_t>(expectedSequence >> 32),
                static_cast<uint32_t>(expectedSequence),
                sequenceMatches ? 1 : 0,
                tlsPayloadHash(frame.payload),
                inboundTlsRecordCount_,
                inboundTlsSequenceAnomalies_
            );
            return false;
        }
        if (read == 0)
            break;
        tlsPlainScratch_.insert(
            tlsPlainScratch_.end(),
            tlsReadScratch_.begin(),
            tlsReadScratch_.begin() + read
        );
    }

    if (!sequenceMatches)
        ++inboundTlsSequenceAnomalies_;
    if (inspection.layoutValid && inspection.hasSequence) {
        inboundTlsSequenceKnown_ = true;
        inboundTlsLastSequence_ = inspection.lastSequence;
        inboundTlsRecordCount_ += inspection.recordCount;
    }

    if (tlsPlainScratch_.empty())
        return true;

    const uint64_t plainRxFirstCompletedAtUs =
        pendingTlsRxFirstCompletedAtUs_;
    const uint64_t plainRxLastCompletedAtUs =
        pendingTlsRxLastCompletedAtUs_;
    pendingTlsRxFirstCompletedAtUs_ = 0;
    pendingTlsRxLastCompletedAtUs_ = 0;
    return processDecryptedFrame(
        frame.channel,
        frame.flags,
        tlsPlainScratch_,
        plainRxFirstCompletedAtUs,
        plainRxLastCompletedAtUs
    );
}

bool DirectSession::processDecryptedFrame(
    uint8_t channel,
    uint8_t flags,
    const std::vector<uint8_t>& plain,
    uint64_t rxFirstCompletedAtUs,
    uint64_t rxLastCompletedAtUs
) {
    const uint8_t frameType = flags & FRAME_TYPE_MASK;
    const uint8_t messageBits = flags & ~FRAME_TYPE_MASK;
    DecryptedFragment& fragment = decryptedFragments_[channel];

    if (frameType == FRAME_BULK) {
        fragment = DecryptedFragment{};
        if (plain.empty())
            return true;
        return processDecryptedMessage(
            channel,
            flags,
            plain.data(),
            plain.size(),
            rxFirstCompletedAtUs,
            rxLastCompletedAtUs
        );
    }

    if (frameType == FRAME_FIRST) {
        fragment.active = true;
        fragment.messageBits = messageBits;
        fragment.rxFirstCompletedAtUs = rxFirstCompletedAtUs;
        fragment.rxLastCompletedAtUs = rxLastCompletedAtUs;
        fragment.payload = plain;
        if (fragment.payload.size() > MAX_LOGICAL_MESSAGE_SIZE)
            return fail(DirectError::MessageTooLarge);
        return true;
    }

    // Match aasdks behavior for a stale/orphan fragments
    if (!fragment.active || fragment.messageBits != messageBits) {
        fragment = DecryptedFragment{};
        return true;
    }

    fragment.payload.insert(fragment.payload.end(), plain.begin(), plain.end());
    if (rxFirstCompletedAtUs != 0 &&
        (fragment.rxFirstCompletedAtUs == 0 ||
         rxFirstCompletedAtUs < fragment.rxFirstCompletedAtUs))
    {
        fragment.rxFirstCompletedAtUs = rxFirstCompletedAtUs;
    }
    if (rxLastCompletedAtUs > fragment.rxLastCompletedAtUs)
        fragment.rxLastCompletedAtUs = rxLastCompletedAtUs;
    if (fragment.payload.size() > MAX_LOGICAL_MESSAGE_SIZE) {
        fragment = DecryptedFragment{};
        return fail(DirectError::MessageTooLarge);
    }

    if (frameType != FRAME_LAST)
        return true;

    std::vector<uint8_t> assembled;
    assembled.swap(fragment.payload);
    const uint64_t assembledRxFirstCompletedAtUs =
        fragment.rxFirstCompletedAtUs;
    const uint64_t assembledRxLastCompletedAtUs =
        fragment.rxLastCompletedAtUs;
    fragment = DecryptedFragment{};
    if (assembled.empty())
        return true;

    return processDecryptedMessage(
        channel,
        messageBits | FRAME_BULK,
        assembled.data(),
        assembled.size(),
        assembledRxFirstCompletedAtUs,
        assembledRxLastCompletedAtUs
    );
}

bool DirectSession::processDecryptedMessage(
    uint8_t channel,
    uint8_t flags,
    const uint8_t* data,
    size_t length,
    uint64_t rxFirstCompletedAtUs,
    uint64_t rxLastCompletedAtUs
) {
    if (length < 2) {
        return fail(DirectError::MissingMessageId);
    }

    const uint16_t messageId = readBe16(data);
    messageRxFirstCompletedAtUs_ = rxFirstCompletedAtUs;
    messageRxLastCompletedAtUs_ = rxLastCompletedAtUs;
    const uint8_t* const payloadData = data + 2;
    const size_t payloadSize = length - 2;

    if (channel == CONTROL_CHANNEL) {
        std::vector<uint8_t> payload(payloadData, payloadData + payloadSize);
        return processControlMessage(messageId, payload);
    }

    // Ignore channels that this head unit does not advertise.
    if (channel > DIRECT_MICROPHONE_CHANNEL)
        return true;
    const bool mediaSink = channel >= DIRECT_VIDEO_CHANNEL &&
                           channel <= DIRECT_SYSTEM_AUDIO_CHANNEL;

    if (messageId == CHANNEL_OPEN_REQUEST) {
        vitaLogInfo("direct-usb: channel open requested channel=%u", channel);
        openedChannels_ |= 1u << channel;
        state_ = DirectState::ChannelsReady;
        return sendEncrypted(
            channel,
            ENCRYPTED_CONTROL_FLAGS,
            CHANNEL_OPEN_RESPONSE,
            statusOk()
        );
    }

    if (mediaSink && messageId == AV_SETUP_REQUEST) {
        if (!sendEncrypted(
                channel,
                ENCRYPTED_SIGNAL_FLAGS,
                AV_SETUP_RESPONSE,
                avSetupOk(mediaMaxUnacked_)))
            return false;
        if (channel == DIRECT_VIDEO_CHANNEL) {
            vitaLogInfo("direct-usb: video setup max_unacked=%u", mediaMaxUnacked_);
            state_ = DirectState::MediaReady;
            return sendEncrypted(
                channel,
                ENCRYPTED_SIGNAL_FLAGS,
                VIDEO_FOCUS_INDICATION,
                videoFocus(true)
            );
        }
        return true;
    }

    if ((mediaSink || channel == DIRECT_MICROPHONE_CHANNEL) &&
        messageId == AV_START_INDICATION) {
        if (channel < 16)
            mediaSessions_[channel] = readFirstVarintField(
                payloadData,
                payloadSize,
                1, // Field 1: media session id.
                0 // Missing session id falls back to 0.
            );
        return true;
    }

    if (messageId == VIDEO_FOCUS_REQUEST && channel == DIRECT_VIDEO_CHANNEL) {
        return sendEncrypted(
            channel,
            ENCRYPTED_SIGNAL_FLAGS,
            VIDEO_FOCUS_INDICATION,
            videoFocus(false)
        );
    }

    if (channel == DIRECT_INPUT_CHANNEL && messageId == INPUT_BINDING_REQUEST) {
        openedChannels_ |= 1u << channel;
        return sendEncrypted(
            channel,
            ENCRYPTED_SIGNAL_FLAGS,
            INPUT_BINDING_RESPONSE,
            statusOk()
        );
    }

    if (channel == DIRECT_SENSOR_CHANNEL && messageId == SENSOR_START_REQUEST) {
        const uint32_t sensorType = readFirstVarintField(
            payloadData,
            payloadSize,
            1, // Field 1: requested sensor type.
            0 // Missing sensor type means no matching event.
        );
        if (!sendEncrypted(
                channel,
                ENCRYPTED_SIGNAL_FLAGS,
                SENSOR_START_RESPONSE,
                statusOk()))
        {
            return false;
        }
        const std::vector<uint8_t> event = sensorEvent(sensorType);
        if (!event.empty()) {
            return sendEncrypted(
                channel,
                ENCRYPTED_SIGNAL_FLAGS,
                SENSOR_EVENT,
                event
            );
        }
        return true;
    }

    if (channel == DIRECT_MICROPHONE_CHANNEL &&
        messageId == MICROPHONE_REQUEST)
    {
        const bool wasOpen = microphoneOpen_;
        const bool open = readFirstVarintField(
            payloadData,
            payloadSize,
            1, // Field 1: microphone open flag.
            0 // Missing open flag means closed.
        ) != 0;
        uint32_t maxUnacked = readFirstVarintField(
            payloadData,
            payloadSize,
            4, // Field 4: phone-advertised max unacknowledged microphone frames.
            1 // Default to one frame in flight when the phone omits the field.
        );
        if (maxUnacked == 0)
            maxUnacked = 1; // Zero would stall microphone transmission.

        microphoneOpen_ = open;
        microphoneMaxUnacked_ = maxUnacked;
        microphoneFramesInFlight_ = 0;

        FixedWriter<16> response;
        if (!response.varintField(1, 0) || // Field 1: status OK.
            !response.varintField(2, MICROPHONE_SESSION_ID)) // Field 2: microphone session id.
        {
            return false;
        }
        vitaLogInfo(
            "direct-usb: microphone %s session=%u max_unacked=%u",
            open ? "open" : "closed",
            MICROPHONE_SESSION_ID,
            microphoneMaxUnacked_
        );
        if (!sendEncryptedRaw(
                channel,
                ENCRYPTED_SIGNAL_FLAGS,
                MICROPHONE_RESPONSE,
                response.data(),
                response.size()))
        {
            return false;
        }

        if (open && !wasOpen) {
            FixedWriter<16> start;
            if (!start.varintField(1, MICROPHONE_SESSION_ID) || // Field 1: microphone session id.
                !start.varintField(2, 0)) // Field 2: start status/flags.
            {
                return false;
            }
            return sendEncryptedRaw(
                channel,
                ENCRYPTED_SIGNAL_FLAGS,
                AV_START_INDICATION,
                start.data(),
                start.size()
            );
        }
        return true;
    }

    if (channel == DIRECT_MICROPHONE_CHANNEL &&
        messageId == AV_STOP_INDICATION)
    {
        microphoneOpen_ = false;
        microphoneFramesInFlight_ = 0;
        vitaLogInfo("direct-usb: microphone stopped by phone");
        return true;
    }

    if (channel == DIRECT_MICROPHONE_CHANNEL && messageId == AV_MEDIA_ACK) {
        uint32_t acknowledged = readFirstVarintField(
            payloadData,
            payloadSize,
            2, // Field 2: acknowledged media frame count.
            1 // Missing ack count means one acknowledged frame.
        );
        if (acknowledged == 0)
            acknowledged = 1; // Treat invalid zero ack as one so the stream can recover.
        const uint32_t consumed = std::min(
            acknowledged,
            microphoneFramesInFlight_
        );
        microphoneFramesInFlight_ -= consumed;
        microphoneAcksReceived_ += consumed;
        return true;
    }

    return !mediaSink || processMedia(channel, messageId, payloadData, payloadSize);
}

bool DirectSession::processControlMessage(
    uint16_t messageId,
    const std::vector<uint8_t>& payload
) {
    if (messageId == SHUTDOWN_RESPONSE) {
        return true;
    }

    if (messageId == SERVICE_DISCOVERY_REQUEST) {
        state_ = DirectState::ServiceReady;
        vitaLogInfo(
            "direct-usb: advertising video=1280x720 fps=30 resolution_enum=%u fps_enum=%u",
            AA_VIDEO_RESOLUTION_720P,
            AA_VIDEO_FRAME_RATE_30_FPS
        );
        vitaLogInfo("direct-usb: music/navigation=%s system-audio=vita",
                    audioOnVita_ ? "vita" : "phone");
        return sendEncrypted(
            CONTROL_CHANNEL,
            ENCRYPTED_SIGNAL_FLAGS,
            SERVICE_DISCOVERY_RESPONSE,
            serviceDiscoveryResponse(audioOnVita_)
        );
    }

    if (messageId == PING_REQUEST)
        return sendEncrypted(CONTROL_CHANNEL, ENCRYPTED_SIGNAL_FLAGS, PING_RESPONSE, payload);

    if (messageId == AUDIO_FOCUS_REQUEST)
        return sendEncrypted(
            CONTROL_CHANNEL,
            ENCRYPTED_SIGNAL_FLAGS,
            AUDIO_FOCUS_RESPONSE,
            audioFocus(payload)
        );

    if (messageId == NAV_FOCUS_REQUEST) {
        std::vector<uint8_t> out;
        writeVarintField(out, 1, 2); // Field 1: navigation focus granted.
        return sendEncrypted(
            CONTROL_CHANNEL,
            ENCRYPTED_SIGNAL_FLAGS,
            NAV_FOCUS_RESPONSE,
            out
        );
    }

    return true;
}

} // namespace android_auto
