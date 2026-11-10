#include "carplay/carplay_sockets.h"

#include "logger/vita_log.h"

#include <psp2/net/net.h>

namespace carplay {
namespace {

int openSocket(const char* name, int type)
{
    const int sock = sceNetSocket(name, SCE_NET_AF_INET, type, 0);
    if (sock < 0)
        return sock;
    const int one = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    return sock;
}

int bindSocket(int sock, uint16_t port)
{
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(port);
    addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    return sceNetBind(sock, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr));
}

} // namespace

int listenOn(const char* name, uint16_t port)
{
    const int sock = openSocket(name, SCE_NET_SOCK_STREAM);
    if (sock < 0)
        return sock;
    int result = bindSocket(sock, port);
    if (result >= 0)
        result = sceNetListen(sock, 2);
    if (result < 0) {
        vitaLog("carplay: listen on %u failed 0x%08x", static_cast<unsigned>(port),
                static_cast<unsigned>(result));
        sceNetSocketClose(sock);
        return result;
    }
    return sock;
}

int udpOn(const char* name, uint16_t port)
{
    const int sock = openSocket(name, SCE_NET_SOCK_DGRAM);
    if (sock < 0)
        return sock;
    const int buffer = 128 * 1024;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &buffer, sizeof(buffer));
    const int result = bindSocket(sock, port);
    if (result < 0) {
        vitaLog("carplay: bind udp %u failed 0x%08x", static_cast<unsigned>(port),
                static_cast<unsigned>(result));
        sceNetSocketClose(sock);
        return result;
    }
    return sock;
}

} // namespace carplay
