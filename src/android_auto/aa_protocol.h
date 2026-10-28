#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

#include "android_auto/aa_errors.h"

namespace android_auto {
// Open Auto Stuff 
constexpr uint8_t CONTROL_CHANNEL = 0;
constexpr uint8_t PLAINTEXT_CONTROL_FLAGS = 0x03;
constexpr uint8_t ENCRYPTED = 0x08;
// Android Auto 1.7 as it is the lastest and easist by the looks
constexpr uint16_t VERSION_REQUEST = 1;
constexpr uint16_t VERSION_RESPONSE = 2;
constexpr uint16_t VERSION_STATUS_MATCH = 0;
constexpr uint16_t SSL_HANDSHAKE = 3;
constexpr size_t HEADER_SIZE = 4;
constexpr size_t MAX_FRAME_SIZE = UINT16_MAX;
constexpr size_t MAX_LOGICAL_MESSAGE_SIZE = 16 * 1024 * 1024;

struct Frame {
    uint8_t channel;
    uint8_t flags;
    // Encrypted frames contain only TLS record bytes. Plaintext frames begin
    // with this two-byte Android Auto message ID.
    bool encrypted;
    uint16_t messageId;
    uint64_t rxFirstCompletedAtUs = 0;
    uint64_t rxLastCompletedAtUs = 0;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encodeFrame(
    uint8_t channel,
    uint8_t flags,
    uint16_t messageId,
    const std::vector<uint8_t>& payload
);

uint16_t readBe16(const uint8_t* data);
void writeBe16(std::vector<uint8_t>& out, uint16_t value);

class FrameParser {
public:
    std::vector<Frame> push(
        const uint8_t* data,
        size_t length,
        uint64_t rxFirstCompletedAtUs = 0,
        uint64_t rxLastCompletedAtUs = 0
    );
    bool failed() const { return failed_; }
    int errorCode() const { return static_cast<int>(errorCode_); }
    const char* errorName() const { return frameParseErrorName(errorCode_); }
    size_t failureBufferedSize() const { return failureBufferedSize_; }
    const std::array<uint8_t, 16>& failureHead() const { return failureHead_; }

private:
    struct PlainFragment {
        bool active = false;
        uint8_t messageBits = 0;
        uint32_t totalSize = 0;
        uint64_t rxFirstCompletedAtUs = 0;
        uint64_t rxLastCompletedAtUs = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<uint8_t> buffer_;
    uint64_t bufferRxFirstCompletedAtUs_ = 0;
    uint64_t bufferRxLastCompletedAtUs_ = 0;
    std::array<PlainFragment, 256> fragments_{};
    bool failed_ = false;
    FrameParseError errorCode_ = FrameParseError::None;
    size_t failureBufferedSize_ = 0;
    std::array<uint8_t, 16> failureHead_{};

    void fail(FrameParseError errorCode);
};

}
