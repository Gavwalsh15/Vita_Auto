#pragma once

#include <cstdint>

namespace android_auto { struct MediaFrame; }
namespace audio { class Output; }

namespace audio {
namespace sink {

struct AudioSinkStats {
    uint32_t mediaFrames = 0;
    uint32_t promptFrames = 0;
    uint32_t rejectedFrames = 0;
    uint64_t mediaBytes = 0;
    uint64_t promptBytes = 0;
};

// Return true for an audio channel, including empty/rejected audio packets.
bool acceptFrame(const android_auto::MediaFrame& frame,
                 audio::Output& media, audio::Output& prompt, AudioSinkStats& stats);
void logStats(const char* prefix, const AudioSinkStats& stats,
              const audio::Output& media, const audio::Output& prompt);

} // namespace sink
} // namespace audio
