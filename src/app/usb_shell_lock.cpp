#include "app/usb_shell_lock.h"
#include "logger/vita_log.h"

#include <psp2/shellutil.h>
#include <algorithm>

namespace app {

bool UsbShellLock::acquire()
{
    error_ = AppError::None;
    if (!initialized_) {
        const int result = sceShellUtilInitEvents(0);
        if (result < 0) {
            error_ = AppError::ShellEventsInitFailed;
            vitaLogAppError(error_, result);
            return false;
        }
        initialized_ = true;
    }

    const SceShellUtilLockType lockTypes[] = {
        SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN,
        SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION,
    };
    return std::all_of(lockTypes, lockTypes + 2, [this](SceShellUtilLockType type) {
        if (held_ & static_cast<unsigned>(type))
            return true;
        const int result = sceShellUtilLock(type);
        if (result < 0) {
            error_ = AppError::ShellLockFailed;
            vitaLogAppError(error_, result);
            return false;
        }
        held_ |= static_cast<unsigned>(type);
        return true;
    });
}

bool UsbShellLock::releaseLock(unsigned type)
{
    if (!(held_ & type))
        return true;
    const int result = sceShellUtilUnlock(static_cast<SceShellUtilLockType>(type));
    if (result < 0) {
        error_ = AppError::ShellUnlockFailed;
        vitaLogAppError(error_, result);
        return false;
    }
    held_ &= ~type;
    return true;
}

bool UsbShellLock::releaseHome()
{
    return releaseLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
}

bool UsbShellLock::release()
{
    const SceShellUtilLockType lockTypes[] = {
        SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION,
        SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN,
    };
    return std::all_of(lockTypes, lockTypes + 2, [this](SceShellUtilLockType type) {
        return releaseLock(static_cast<unsigned>(type));
    });
}

} // namespace app
