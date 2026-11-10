#pragma once

namespace carplay {

// An iPhone that finds nothing it wants in the offer opens no audio stream and plays
// on its own speaker.
enum class AudioOffer {
    Basic,
    Opus,
    Full,
};

inline const char* audioOfferName(AudioOffer offer)
{
    switch (offer) {
    case AudioOffer::Basic: return "basic";
    case AudioOffer::Opus: return "opus";
    case AudioOffer::Full: break;
    }
    return "full";
}

} // namespace carplay
