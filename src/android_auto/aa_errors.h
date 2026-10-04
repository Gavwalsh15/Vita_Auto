#pragma once

namespace android_auto {

enum class DirectError : int {
    None = 0,
    InvalidOutboundFrame = -1,  // Empty frame or TLS frame exceeding the wire limit.
    EncryptedSendRejected = -2, // TLS not ready, null payload, or oversized payload.
    TlsWriteFailed = -3,
    VersionNegotiationFailed = -4,
    TlsStartFailed = -5,
    TlsHandshakeFeedFailed = -6,
    TlsRecordFeedFailed = -7,
    TlsReadFailed = -8,
    MissingMessageId = -9,      // Decrypted message has fewer than two bytes.
    FrameParseFailed = -10,     // See the accompanying FrameParseError in the log.
    MessageTooLarge = -11,      // Reassembled message exceeds the logical limit.
    InvalidInput = -12,         // Nonempty input requires a data buffer.
};

inline const char* directErrorName(DirectError error)
{
    switch (error) {
    case DirectError::None:                     return "None";
    case DirectError::InvalidOutboundFrame:     return "InvalidOutboundFrame";
    case DirectError::EncryptedSendRejected:    return "EncryptedSendRejected";
    case DirectError::TlsWriteFailed:           return "TlsWriteFailed";
    case DirectError::VersionNegotiationFailed: return "VersionNegotiationFailed";
    case DirectError::TlsStartFailed:           return "TlsStartFailed";
    case DirectError::TlsHandshakeFeedFailed:   return "TlsHandshakeFeedFailed";
    case DirectError::TlsRecordFeedFailed:      return "TlsRecordFeedFailed";
    case DirectError::TlsReadFailed:            return "TlsReadFailed";
    case DirectError::MissingMessageId:         return "MissingMessageId";
    case DirectError::FrameParseFailed:         return "FrameParseFailed";
    case DirectError::MessageTooLarge:          return "MessageTooLarge";
    case DirectError::InvalidInput:             return "InvalidInput";
    }
    return "UnknownDirectError";
}

// Parser codes occupy their own domain - they are not DirectError values.
enum class FrameParseError : int {
    None = 0,
    InvalidFlags = 1,
    InvalidLength = 2,
    OrphanFragment = 3,
    FragmentFlagsMismatch = 4,
    MessageTooLarge = 5,
    FinalLengthMismatch = 6,
};

inline const char* frameParseErrorName(FrameParseError error)
{
    switch (error) {
    case FrameParseError::None:                  return "None";
    case FrameParseError::InvalidFlags:          return "InvalidFlags";
    case FrameParseError::InvalidLength:         return "InvalidLength";
    case FrameParseError::OrphanFragment:        return "OrphanFragment";
    case FrameParseError::FragmentFlagsMismatch: return "FragmentFlagsMismatch";
    case FrameParseError::MessageTooLarge:       return "MessageTooLarge";
    case FrameParseError::FinalLengthMismatch:   return "FinalLengthMismatch";
    }
    return "UnknownFrameParseError";
}

} // namespace android_auto
