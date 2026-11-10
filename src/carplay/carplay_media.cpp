#include "carplay/carplay_session_impl.h"

#include "inputs/vita_input.h"
#include "logger/vita_log.h"
#include "video/video_pipeline.h"

namespace carplay {
namespace {

constexpr uint32_t AUDIO_BUFFER_MS = 250;

bool hasIdr(const uint8_t* data, size_t size)
{
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if ((data[i + 3] & 0x1f) == 5)
                return true;
            i += 2;
        }
    }
    return false;
}

} // namespace

void Session::Impl::on_config(nbtplay::VideoCodec codec, const uint8_t* record, size_t length)
{
    ++videoConfigs;
    if (codec != nbtplay::VideoCodec::H264) {
        vitaLog("carplay: video codec is not H.264; the Vita cannot decode it");
        return;
    }
    Bytes annexB;
    if (!nbtplay::avcc_to_annex_b(record, length, annexB)) {
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
    const bool key = sendParameterSets || hasIdr(annexB, length);
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
    const int index = streamType == nbtplay::kStreamAltAudio ? 1
                    : streamType == nbtplay::kStreamMainHighAudio ? 2 : 0;
    AudioStream& stream = audioStreams[index];
    if (conn == nullptr)
        return;
    const nbtplay::AudioFormatInfo format =
        conn->media.audio(nbtplay::audio_channel(streamType)).format();
    const bool pcm = format.codec == nbtplay::AudioCodec::Pcm;
    const bool aac = format.codec == nbtplay::AudioCodec::AacLc;
    if ((!pcm && !aac) || (format.channels != 1 && format.channels != 2)) {
        if (++stream.dropped == 1)
            vitaLog("carplay: audio stream %d is neither PCM nor AAC-LC; it stays silent", streamType);
        return;
    }
    if (!stream.output || stream.rate != format.sample_rate || stream.channels != format.channels) {
        stream.output.reset(new audio::Output(format.sample_rate,
                                              static_cast<uint8_t>(format.channels), AUDIO_BUFFER_MS));
        stream.aac.reset();
        stream.rate = format.sample_rate;
        stream.channels = static_cast<uint8_t>(format.channels);
        vitaLog("carplay: audio stream %d: %s, %u Hz, %u channel(s)", streamType, aac ? "AAC-LC" : "PCM",
                static_cast<unsigned>(format.sample_rate), static_cast<unsigned>(format.channels));
    }
    static std::vector<int16_t> samples;
    if (aac) {
        if (!stream.aac) {
            stream.aac.reset(new audio::AacDecoder());
            if (!stream.aac->open(format.sample_rate, static_cast<uint8_t>(format.channels)))
                vitaLog("carplay: AAC decoder did not start (0x%08x)",
                        static_cast<unsigned>(stream.aac->lastError()));
        }
        if (!stream.aac->isOpen())
            return;
        if (stream.aac->decode(payload, length, samples) <= 0) {
            if (++stream.dropped <= 3)
                vitaLog("carplay: AAC decode failed (0x%08x), %u bytes",
                        static_cast<unsigned>(stream.aac->lastError()), static_cast<unsigned>(length));
            return;
        }
        stream.output->pushPcm16(samples.data(), samples.size());
        ++stream.packets;
        return;
    }
    // Big-endian 16-bit samples to the Vita's native order.
    const size_t count = length / 2;
    samples.resize(count);
    for (size_t i = 0; i < count; ++i)
        samples[i] = static_cast<int16_t>((payload[2 * i] << 8) | payload[2 * i + 1]);
    stream.output->pushPcm16(samples.data(), count);
    ++stream.packets;
}

} // namespace carplay
