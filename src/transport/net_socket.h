#pragma once

#include <cstdint>

namespace net_socket {

bool setBuffers(int sock, int receiveBytes, int sendBytes);

// Sockets accepted from the listener inherit its buffer sizes.
int listenTcp(const char* name, uint16_t port, int backlog, int receiveBytes = 0, int sendBytes = 0);
int bindUdp(const char* name, uint16_t port, int receiveBytes = 0);

} // namespace net_socket
