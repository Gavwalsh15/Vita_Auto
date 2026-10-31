#include "transport/transport.h"

#include "logger/vita_log.h"

namespace {
Transport::WakeHandler g_wakeHandler = nullptr;
}

bool Transport::fail(app::AppError error, int detail)
{
    if (lastAppError_ == app::AppError::None) {
        lastAppError_ = error;
        lastError_ = detail;
    }
    vitaLogAppError(error, detail);
    return false;
}

void Transport::setWakeHandler(WakeHandler handler)
{
    __atomic_store_n(&g_wakeHandler, handler, __ATOMIC_RELEASE);
}

void Transport::wake()
{
    const WakeHandler handler = __atomic_load_n(&g_wakeHandler, __ATOMIC_ACQUIRE);
    if (handler != nullptr)
        handler();
}
