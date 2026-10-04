#pragma once

#include "app/app_errors.h"

namespace app {

enum class ConnectionScreen { WaitingForPhone, Connecting, Disconnecting, Retrying, Disconnected, Error, RestartRequired };

void showConnectionScreen(ConnectionScreen screen, AppError error, unsigned retrySeconds,
                          bool audioOnVita, bool audioSettingSaved);

} // namespace app
