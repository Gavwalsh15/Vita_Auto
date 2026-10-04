#pragma once

#include "app/app_errors.h"

namespace app {

// Explicit release keeps HOME locked if USB cleanup fails.
class UsbShellLock {
public:
    bool acquire();
    // Keep shell USB handling suppressed on the disconnected screen.
    bool releaseHome();
    bool release();
    AppError lastError() const { return error_; }

private:
    bool releaseLock(unsigned type);
    bool initialized_ = false;
    unsigned held_ = 0;
    AppError error_ = AppError::None;
};

} // namespace app
