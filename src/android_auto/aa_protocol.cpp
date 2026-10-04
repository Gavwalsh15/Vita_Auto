#include "android_auto/aa_protocol.h"

#include <algorithm>
#include <utility>

namespace android_auto {

namespace {

constexpr uint8_t FRAME_TYPE_MASK = 0x03;
constexpr uint8_t FRAME_FIRST = 0x01;
constexpr uint8_t FRAME_LAST = 0x02;
constexpr uint8_t FRAME_BULK = FRAME_FIRST | FRAME_LAST;
constexpr size_t EXTENDED_HEADER_SIZE = 8;

uint32_t readU32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

}

uint16_t readBe16(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

void writeBe16(std::vector<uint8_t>& output, uint16_t value)
{
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    output.push_back(static_cast<uint8_t>(value & 0xff));
}

void FrameParser::fail(FrameParseError errorCode)
{
    errorCode_ = errorCode;
    failureBufferedSize_ = buffer_.size();
    failureHead_.fill(0);
    const size_t copySize = std::min(buffer_.size(), failureHead_.size());
    std::copy_n(buffer_.begin(), copySize, failureHead_.begin());
    failed_ = true;
    buffer_.clear();
}

std::vector<uint8_t> encodeFrame(
    uint8_t channel,
    uint8_t flags,
    uint16_t messageId,
    const std::vector<uint8_t>& payload
) {
    std::vector<uint8_t> frame;
    if (payload.size() > MAX_FRAME_SIZE - sizeof(messageId))
        return frame;
    const size_t bodyLength = sizeof(messageId) + payload.size();

    frame.reserve(HEADER_SIZE + bodyLength);
    frame.push_back(channel);
    frame.push_back(flags);
    writeBe16(frame, static_cast<uint16_t>(bodyLength));
    writeBe16(frame, messageId);
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

std::vector<Frame> FrameParser::push(
    const uint8_t* data,
    size_t length,
    uint64_t rxFirstCompletedAtUs,
    uint64_t rxLastCompletedAtUs
)
{
    std::vector<Frame> frames;

    if (failed_)
        return frames;

    if (data != nullptr && length > 0) {
        if (buffer_.empty())
            bufferRxFirstCompletedAtUs_ = rxFirstCompletedAtUs;
        else if (rxFirstCompletedAtUs != 0 &&
                 (bufferRxFirstCompletedAtUs_ == 0 ||
                  rxFirstCompletedAtUs < bufferRxFirstCompletedAtUs_))
        {
            bufferRxFirstCompletedAtUs_ = rxFirstCompletedAtUs;
        }
        if (rxLastCompletedAtUs > bufferRxLastCompletedAtUs_)
            bufferRxLastCompletedAtUs_ = rxLastCompletedAtUs;
        buffer_.insert(buffer_.end(), data, data + length);
    }

    while (buffer_.size() >= HEADER_SIZE)
    {
        const uint8_t channel = buffer_[0];
        const uint8_t flags = buffer_[1];
        const uint8_t frameType = flags & FRAME_TYPE_MASK;
        const bool encrypted = (buffer_[1] & ENCRYPTED) != 0;
        const size_t headerSize = frameType == FRAME_FIRST
            ? EXTENDED_HEADER_SIZE
            : HEADER_SIZE;

        if ((flags & ~0x0f) != 0) {
            fail(FrameParseError::InvalidFlags); // invalid flag bits
            break;
        }

        if (buffer_.size() < headerSize)
            break;

        const uint16_t bodyLength = readBe16(buffer_.data() + 2);
        const uint32_t totalSize = frameType == FRAME_FIRST
            ? readU32(buffer_.data() + 4)
            : bodyLength;
        if (bodyLength == 0 ||
            (frameType == FRAME_FIRST &&
             (totalSize == 0 || totalSize > MAX_LOGICAL_MESSAGE_SIZE)))
        {
            fail(FrameParseError::InvalidLength); // invalid frame or extended message length
            break;
        }

        const size_t frameLength = headerSize + bodyLength;

        if (buffer_.size() < frameLength)
            break;

        std::vector<uint8_t> body(
            buffer_.begin() + headerSize,
            buffer_.begin() + frameLength
        );
        const uint64_t frameRxFirstCompletedAtUs =
            bufferRxFirstCompletedAtUs_;
        const uint64_t frameRxLastCompletedAtUs =
            bufferRxLastCompletedAtUs_;
        buffer_.erase(buffer_.begin(), buffer_.begin() + frameLength);
        if (buffer_.empty()) {
            bufferRxFirstCompletedAtUs_ = 0;
            bufferRxLastCompletedAtUs_ = 0;
        } else {
            bufferRxFirstCompletedAtUs_ = rxFirstCompletedAtUs;
            bufferRxLastCompletedAtUs_ = rxLastCompletedAtUs;
        }

        if (encrypted)
        {
            Frame frame{};
            frame.channel = channel;
            frame.flags = flags;
            frame.encrypted = true;
            frame.messageId = 0;
            frame.rxFirstCompletedAtUs = frameRxFirstCompletedAtUs;
            frame.rxLastCompletedAtUs = frameRxLastCompletedAtUs;
            frame.payload.swap(body);
            frames.push_back(std::move(frame));
            continue;
        }

        PlainFragment& fragment = fragments_[channel];
        if (frameType == FRAME_BULK)
        {
            fragment = PlainFragment{};
            if (body.size() < sizeof(uint16_t))
                continue;

            Frame frame{};
            frame.channel = channel;
            frame.flags = flags;
            frame.encrypted = false;
            frame.messageId = readBe16(body.data());
            frame.rxFirstCompletedAtUs = frameRxFirstCompletedAtUs;
            frame.rxLastCompletedAtUs = frameRxLastCompletedAtUs;
            frame.payload.assign(body.begin() + sizeof(uint16_t), body.end());
            frames.push_back(std::move(frame));
            continue;
        }

        const uint8_t messageBits = flags & ~FRAME_TYPE_MASK;
        if (frameType == FRAME_FIRST) {
            fragment.active = true;
            fragment.messageBits = messageBits;
            fragment.totalSize = totalSize;
            fragment.rxFirstCompletedAtUs = frameRxFirstCompletedAtUs;
            fragment.rxLastCompletedAtUs = frameRxLastCompletedAtUs;
            fragment.payload.swap(body);
            continue;
        }

        if (!fragment.active || fragment.messageBits != messageBits) {
            fail(!fragment.active ? FrameParseError::OrphanFragment
                                  : FrameParseError::FragmentFlagsMismatch);
            break;
        }

        fragment.payload.insert(fragment.payload.end(), body.begin(), body.end());
        if (frameRxFirstCompletedAtUs != 0 &&
            (fragment.rxFirstCompletedAtUs == 0 ||
             frameRxFirstCompletedAtUs < fragment.rxFirstCompletedAtUs))
        {
            fragment.rxFirstCompletedAtUs = frameRxFirstCompletedAtUs;
        }
        if (frameRxLastCompletedAtUs > fragment.rxLastCompletedAtUs)
            fragment.rxLastCompletedAtUs = frameRxLastCompletedAtUs;
        if (fragment.payload.size() > MAX_LOGICAL_MESSAGE_SIZE) {
            fail(FrameParseError::MessageTooLarge); /* accumulated plaintext fragment is too large */
            break;
        }

        if (frameType == FRAME_LAST) {
            if (fragment.payload.size() != fragment.totalSize ||
                fragment.payload.size() < sizeof(uint16_t))
            {
                fail(FrameParseError::FinalLengthMismatch); // final plaintext size differs from FIRST
                break;
            }

            Frame frame{};
            frame.channel = channel;
            frame.flags = messageBits | FRAME_BULK;
            frame.encrypted = false;
            frame.messageId = readBe16(fragment.payload.data());
            frame.rxFirstCompletedAtUs = fragment.rxFirstCompletedAtUs;
            frame.rxLastCompletedAtUs = fragment.rxLastCompletedAtUs;
            frame.payload.assign(
                fragment.payload.begin() + sizeof(uint16_t),
                fragment.payload.end()
            );
            frames.push_back(std::move(frame));
            fragment = PlainFragment{};
        }
    }

    return frames;
}

}
