#pragma once

#include <cstdint>

namespace carplay {

// Non-blocking IPv4 sockets bound to a fixed port. Return the socket, or a negative error.
int listenOn(const char* name, uint16_t port);
int udpOn(const char* name, uint16_t port);

} // namespace carplay
