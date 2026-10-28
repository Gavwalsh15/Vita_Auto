#pragma once

#include "app/app_errors.h"
#include "app/settings.h"

namespace app {
namespace direct_session {

enum class Status { WaitingForPhone, Connecting };

// Main-thread controls for the single IO owner. Join before starting again.
bool start(const Settings& settings);
void requestStop();
Status status();
bool finished();
void join();
// False after USB cleanup fails: do not reuse retained kernel resources.
bool canRestart();
// Stable after start() fails or finished() returns true. Cleanup preserves the
// first failure; SDK/protocol details are also written at the failure site.
AppError lastError();
int lastErrorDetail();
const char* lastErrorName();

} // namespace direct_session
} // namespace app
