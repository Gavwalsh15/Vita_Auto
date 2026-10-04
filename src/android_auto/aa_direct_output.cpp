#include "android_auto/aa_direct.h"

#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "android_auto/aa_direct_messages.h"

namespace android_auto {
namespace {

constexpr uint8_t ENCRYPTED_SIGNAL_FLAGS = 0x0b;
constexpr uint16_t AV_MEDIA_WITH_TIMESTAMP = 0x0000;
constexpr uint16_t INPUT_REPORT = 0x8001;
constexpr uint16_t INPUT_MAX_X = 1279;
constexpr uint16_t INPUT_MAX_Y = 719;
constexpr size_t MEDIA_TIMESTAMP_SIZE = 8;
constexpr size_t MICROPHONE_SAMPLES_PER_FRAME = 512;

} // namespace

bool DirectSession::inputReady() const
{
    return (openedChannels_ & (1u << DIRECT_INPUT_CHANNEL)) != 0;
}

bool DirectSession::sendPingRequest()
{
    if (!tls_ || !tls_->secure() || state_ == DirectState::Error)
        return false;
    constexpr uint16_t PING_REQUEST = 0x000b;
    FixedWriter<16> request;
    return request.varintField(1, sceKernelGetSystemTimeWide()) &&
        sendEncryptedRaw(CONTROL_CHANNEL, ENCRYPTED_SIGNAL_FLAGS, PING_REQUEST,
                         request.data(), request.size());
}

bool DirectSession::sendShutdownRequest()
{
    if (!tls_ || !tls_->secure() || state_ == DirectState::Error)
        return false;
    constexpr uint16_t SHUTDOWN_REQUEST = 0x000f;
    FixedWriter<8> request;
    return request.varintField(1, 1) && // User selected disconnect.
        sendEncryptedRaw(CONTROL_CHANNEL, ENCRYPTED_SIGNAL_FLAGS, SHUTDOWN_REQUEST,
                         request.data(), request.size());
}

bool DirectSession::sendTouch(
    TouchAction action,
    uint16_t x,
    uint16_t y,
    uint32_t pointerId
) {
    if (!inputReady())
        return true;

    FixedWriter<32> pointer;
    FixedWriter<48> touch;
    FixedWriter<64> report;
    if (!pointer.varintField(1, std::min<uint16_t>(x, INPUT_MAX_X)) || // Field 1: X coordinate.
        !pointer.varintField(2, std::min<uint16_t>(y, INPUT_MAX_Y)) || // Field 2: Y coordinate.
        !pointer.varintField(3, pointerId) || // Field 3: pointer id.
        !touch.bytesField(1, pointer.data(), pointer.size()) || // Field 1: pointer data.
        !touch.varintField(2, 0) || // Field 2: display id.
        !touch.varintField(3, static_cast<uint8_t>(action)) || // Field 3: touch action.
        !report.varintField(1, sceKernelGetSystemTimeWide()) || // Field 1: event timestamp.
        !report.bytesField(3, touch.data(), touch.size())) // Field 3: touch event.
    {
        return false;
    }

    return sendEncryptedRaw(
        DIRECT_INPUT_CHANNEL,
        ENCRYPTED_SIGNAL_FLAGS,
        INPUT_REPORT,
        report.data(),
        report.size()
    );
}

bool DirectSession::sendButton(uint32_t keycode, bool down)
{
    if (!inputReady())
        return true;

    FixedWriter<32> key;
    FixedWriter<40> keyEvent;
    FixedWriter<64> report;
    if (!key.varintField(1, keycode) || // Field 1: Android keycode.
        !key.varintField(2, down ? 1 : 0) || // Field 2: key down/up state.
        !key.varintField(3, 0) || // Field 3: modifier flags.
        !key.varintField(4, 0) || // Field 4: long press.
        !keyEvent.bytesField(1, key.data(), key.size()) || // Field 1: key event data.
        !report.varintField(1, sceKernelGetSystemTimeWide()) || // Field 1: event timestamp.
        !report.bytesField(4, keyEvent.data(), keyEvent.size())) // Field 4: key event.
    {
        return false;
    }

    return sendEncryptedRaw(
        DIRECT_INPUT_CHANNEL,
        ENCRYPTED_SIGNAL_FLAGS,
        INPUT_REPORT,
        report.data(),
        report.size()
    );
}

bool DirectSession::sendRotary(int32_t delta)
{
    if (!inputReady() || delta == 0)
        return true;

    constexpr uint32_t ROTARY_CONTROLLER_KEYCODE = 65536;
    FixedWriter<16> step;
    FixedWriter<24> relative;
    FixedWriter<48> report;
    if (!step.varintField(1, ROTARY_CONTROLLER_KEYCODE) ||
        !step.varintField(2, static_cast<uint64_t>(static_cast<int64_t>(delta))) ||
        !relative.bytesField(1, step.data(), step.size()) ||
        !report.varintField(1, sceKernelGetSystemTimeWide()) ||
        !report.bytesField(6, relative.data(), relative.size()))
    {
        return false;
    }

    return sendEncryptedRaw(
        DIRECT_INPUT_CHANNEL,
        ENCRYPTED_SIGNAL_FLAGS,
        INPUT_REPORT,
        report.data(),
        report.size()
    );
}

bool DirectSession::sendMicrophone(
    const int16_t* samples,
    size_t sampleCount,
    uint64_t timestampUs
)
{
    if (!microphoneCanSend())
        return true;
    if (samples == nullptr || sampleCount == 0 ||
        sampleCount > MICROPHONE_SAMPLES_PER_FRAME)
    {
        return false;
    }

    uint8_t payload[
        MEDIA_TIMESTAMP_SIZE +
        (MICROPHONE_SAMPLES_PER_FRAME * sizeof(int16_t))
    ];
    writeBe64(payload, timestampUs);
    const size_t pcmBytes = sampleCount * sizeof(int16_t);
    std::memcpy(payload + MEDIA_TIMESTAMP_SIZE, samples, pcmBytes);

    if (!sendEncryptedRaw(
            DIRECT_MICROPHONE_CHANNEL,
            ENCRYPTED_SIGNAL_FLAGS,
            AV_MEDIA_WITH_TIMESTAMP,
            payload,
            MEDIA_TIMESTAMP_SIZE + pcmBytes))
    {
        return false;
    }

    ++microphoneFramesInFlight_;
    ++microphoneFramesSent_;
    return true;
}

void DirectSession::setMediaHandler(MediaHandler handler)
{
    mediaHandler_ = std::move(handler);
}

} // namespace android_auto
