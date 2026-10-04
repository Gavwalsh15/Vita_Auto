#pragma once

#include "app/app_errors.h"

namespace app {

enum class Recovery { ManualRetry, AutomaticRetry, RestartRequired };

// Retry connection failures when cleanup has left USB ready for another attempt.
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
    case AppError::UsbReadFailed: return Recovery::AutomaticRetry;
    default: return Recovery::ManualRetry;
    }
}

} // namespace app
