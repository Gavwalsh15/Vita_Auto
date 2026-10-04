#include "audio/audio_sink.h"
#include "android_auto/aa_direct.h"
#include "audio/audio_output.h"
#include "logger/vita_log.h"

namespace audio {
namespace sink {

bool acceptFrame(
    const android_auto::MediaFrame& frame,
    audio::Output& mediaOutput,
    audio::Output& promptOutput,
    AudioSinkStats& stats
)
{
    if (frame.channel != android_auto::DIRECT_MEDIA_AUDIO_CHANNEL &&
        frame.channel != android_auto::DIRECT_GUIDANCE_AUDIO_CHANNEL &&
        frame.channel != android_auto::DIRECT_SYSTEM_AUDIO_CHANNEL)
    {
        return false;
    }

    if (frame.data == nullptr || frame.size == 0)
        return true;

    // AA is signed little-endian PCM16 so vita no changes.
    if ((frame.size & 1u) != 0) {
        ++stats.rejectedFrames;
        return true;
    }

    const int16_t* const samples =
        reinterpret_cast<const int16_t*>(frame.data);
    const size_t sampleCount = frame.size / sizeof(int16_t);
    const bool isMedia = frame.channel == android_auto::DIRECT_MEDIA_AUDIO_CHANNEL;
    audio::Output& output = isMedia ? mediaOutput : promptOutput;
    const bool accepted = output.pushPcm16(samples, sampleCount);
    if (accepted) {
        if (isMedia) {
            ++stats.mediaFrames;
            stats.mediaBytes += frame.size;
        } else {
            ++stats.promptFrames;
            stats.promptBytes += frame.size;
        }
    } else {
        ++stats.rejectedFrames;
        if (stats.rejectedFrames == 1) {
            vitaLog(
                "audio output failed channel=%u bytes=%u err=0x%08x",
                frame.channel,
                static_cast<unsigned>(frame.size),
                output.lastError()
            );
        }
    }
    return true;
}

void logStats(
    const char* prefix,
    const AudioSinkStats& stats,
    const audio::Output& mediaOutput,
    const audio::Output& promptOutput
)
{
    if (!VITA_AUTO_VERBOSE_LOGGING && stats.rejectedFrames == 0 &&
        mediaOutput.lastError() == 0 && promptOutput.lastError() == 0)
        return;
    vitaLog(
        "%s: audio media=%u/%lluB prompts=%u/%lluB rejected=%u errors=0x%08x (%s)/0x%08x (%s)",
        prefix,
        stats.mediaFrames,
        static_cast<unsigned long long>(stats.mediaBytes),
        stats.promptFrames,
        static_cast<unsigned long long>(stats.promptBytes),
        stats.rejectedFrames,
        mediaOutput.lastError(), mediaOutput.lastErrorName(),
        promptOutput.lastError(), promptOutput.lastErrorName()
    );
}

} // namespace sink
} // namespace audio
