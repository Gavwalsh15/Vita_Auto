#include "carplay/carplay_session_impl.h"

#include "inputs/vita_input.h"
#include "logger/vita_log.h"
#include "video/h264_annexb.h"
#include "video/video_pipeline.h"

namespace carplay {
namespace {

constexpr uint32_t AUDIO_BUFFER_MS = 250;

} // namespace

void Session::Impl::on_config(core::VideoCodec codec, const uint8_t* record, size_t length)
{
    ++videoConfigs;
    if (codec != core::VideoCodec::H264) {
        vitaLog("carplay: video codec is not H.264; the Vita cannot decode it");
        return;
    }
    Bytes annexB;
    if (!core::avcc_to_annex_b(record, length, annexB)) {
        vitaLog("carplay: video config record is malformed");
        return;
    }
    parameterSets.assign(annexB.data(), annexB.data() + annexB.size());
    sendParameterSets = true;
    vitaLog("carplay: video config received (%u bytes of parameter sets)",
            static_cast<unsigned>(parameterSets.size()));
}

// Frames go to the same pipeline Android Auto's video uses. The phone sends the
// SPS and PPS once, in the config; the decoder wants them in front of every key
// frame.
void Session::Impl::on_frame(const uint8_t* annexB, size_t length, uint64_t senderNanos)
{
    if (length == 0)
        return;
    const bool key = sendParameterSets || video::containsIdr(annexB, length);
    frameBuffer.clear();
    if (key && !parameterSets.empty())
        frameBuffer.insert(frameBuffer.end(), parameterSets.begin(), parameterSets.end());
    frameBuffer.insert(frameBuffer.end(), annexB, annexB + length);
    sendParameterSets = false;

    video::pipeline::Frame frame;
    frame.timestamp = senderNanos / 1000;
    frame.data = frameBuffer.data();
    frame.size = frameBuffer.size();
    frame.rxFirstCompletedAtUs = nowUs();
    frame.rxLastCompletedAtUs = frame.rxFirstCompletedAtUs;
    frame.parsedAtUs = frame.rxFirstCompletedAtUs;
    video::pipeline::acceptFrame(frame, videoSequence, videoFrames, "carplay");
    if (!videoLogged) {
        videoLogged = true;
        vitaLog("carplay: first video frame (%u bytes%s)", static_cast<unsigned>(length),
                key ? ", key frame" : "");
        inputs::setReady(true); // Touch starts once there is a picture to touch.
    }
}

void Session::Impl::on_audio(int streamType, uint32_t, const uint8_t* payload, size_t length)
{
    if (conn == nullptr)
        return;
    const int index = streamType == core::kStreamAltAudio ? 1
                    : streamType == core::kStreamMainHighAudio ? 2 : 0;
    const core::AudioFormatInfo format =
        conn->media.audio(core::audio_channel(streamType)).format();
    // CarPlay's PCM is big-endian.
    const audio::Encoding encoding =
        format.codec == core::AudioCodec::AacLc ? audio::Encoding::AacLc : audio::Encoding::Pcm16Be;
    const bool playable = format.codec == core::AudioCodec::Pcm || format.codec == core::AudioCodec::AacLc;
    if (!playable || !audioStreams[index].configure(encoding, format.sample_rate,
                                                    static_cast<uint8_t>(format.channels), AUDIO_BUFFER_MS)) {
        if (++audioUnsupported == 1)
            vitaLog("carplay: audio stream %d is %s, which the Vita does not decode yet; it stays silent",
                    streamType, format.codec == core::AudioCodec::Opus ? "Opus" : "an unknown format");
        return;
    }
    audioStreams[index].push(payload, length);
}

} // namespace carplay
