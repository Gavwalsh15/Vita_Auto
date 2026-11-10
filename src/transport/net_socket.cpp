#include "transport/net_socket.h"

#include "logger/vita_log.h"

#include <psp2/net/net.h>

namespace net_socket {
namespace {

int openBound(const char* name, int type, uint16_t port, int receiveBytes, int sendBytes)
{
    const int sock = sceNetSocket(name, SCE_NET_AF_INET, type, 0);
    if (sock < 0) {
        vitaLog("net: %s socket failed 0x%08x", name, static_cast<unsigned>(sock));
        return sock;
    }
    const int one = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    if (!setBuffers(sock, receiveBytes, sendBytes))
        vitaLog("net: %s buffer sizes not applied", name);
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(port);
    addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    const int result = sceNetBind(sock, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr));
    if (result < 0) {
        vitaLog("net: %s bind to port %u failed 0x%08x", name, static_cast<unsigned>(port),
                static_cast<unsigned>(result));
        sceNetSocketClose(sock);
        return result;
    }
    return sock;
}

} // namespace

bool setBuffers(int sock, int receiveBytes, int sendBytes)
{
    bool ok = true;
    if (receiveBytes > 0)
        ok = sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &receiveBytes,
                              sizeof(receiveBytes)) >= 0;
    if (sendBytes > 0)
        ok = sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &sendBytes,
                              sizeof(sendBytes)) >= 0 && ok;
    return ok;
}

int listenTcp(const char* name, uint16_t port, int backlog, int receiveBytes, int sendBytes)
{
    const int sock = openBound(name, SCE_NET_SOCK_STREAM, port, receiveBytes, sendBytes);
    if (sock < 0)
        return sock;
    const int result = sceNetListen(sock, backlog);
    if (result < 0) {
        vitaLog("net: %s listen on port %u failed 0x%08x", name, static_cast<unsigned>(port),
                static_cast<unsigned>(result));
        sceNetSocketClose(sock);
        return result;
    }
    return sock;
}

int bindUdp(const char* name, uint16_t port, int receiveBytes)
{
    return openBound(name, SCE_NET_SOCK_DGRAM, port, receiveBytes, 0);
}

} // namespace net_socket
