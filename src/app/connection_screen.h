#pragma once

#include "app/app_errors.h"
#include "app/settings.h"

namespace app {

enum class ConnectionScreen { WaitingForPhone, Connecting, Disconnecting, Retrying, Disconnected, Error, RestartRequired };

void showConnectionScreen(ConnectionScreen screen, AppError error, unsigned retrySeconds,
                          bool audioOnVita, bool settingsSaved, ConnectionMode mode,
                          bool canChangeMode = true);

} // namespace app
