#pragma once

#include "app/app_errors.h"

namespace app {

enum class Recovery { ManualRetry, AutomaticRetry, RestartRequired };

// Retry connection failures when cleanup has left the transport ready for
// another attempt. A corrupted feed (TLS MAC / frame parse) is a dead session,
// not a dead USB link: tearing the session down and negotiating a fresh one
// recovers it, so retry instead of parking on the error screen until the user
// presses CROSS.
inline Recovery recoveryAfterSession(AppError error, bool cleanupComplete)
{
    if (!cleanupComplete)
        return Recovery::RestartRequired;

    switch (error) {
    case AppError::None:
    case AppError::UsbConnectTimedOut:
    case AppError::AndroidAutoStartTimedOut:
    case AppError::UsbEnumerationFailed:
    case AppError::UsbSendRejected:
    case AppError::UsbWriteFailed:
    case AppError::UsbWriteTimedOut:
    case AppError::UsbReadFailed:
    case AppError::AndroidAutoFeedFailed:
    case AppError::AndroidAutoSessionFailed:
    case AppError::NetWriteFailed:
    case AppError::NetWriteTimedOut:
    case AppError::NetReadFailed:
    case AppError::NetPeerClosed: return Recovery::AutomaticRetry;
    default: return Recovery::ManualRetry;
    }
}

} // namespace app
