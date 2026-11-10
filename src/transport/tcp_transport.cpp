#include "transport/tcp_transport.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "logger/vita_log.h"

namespace {

// Android Auto's "Headunit Server" listens here on the phone.
constexpr uint16_t HEADUNIT_SERVER_PORT = 5277;
// Socket buffers come out of this pool, so it has to be bigger than the buffers.
constexpr int NET_POOL_SIZE = 1024 * 1024;
constexpr int RECEIVE_BUFFER_SIZE = 256 * 1024;
constexpr int SEND_BUFFER_SIZE = 64 * 1024;
constexpr size_t RECEIVE_DRAIN_LIMIT = 128 * 1024;
// Gaps worth logging, and the least time between two log lines.
constexpr uint64_t RX_GAP_LOG_US = 100000;
constexpr uint64_t LOOP_GAP_LOG_US = 50000;
constexpr uint64_t GAP_LOG_INTERVAL_US = 500000;
constexpr uint64_t CONNECT_ATTEMPT_US = 3000000;
constexpr uint64_t CONNECT_RETRY_DELAY_US = 1000000;
constexpr uint64_t WIFI_CHECK_INTERVAL_US = 1000000;
constexpr uint32_t POLL_INTERVAL_US = 100000;
// Short but non-zero: a zero timeout may mean "wait forever" in some SDK calls.
constexpr uint32_t CONNECT_POLL_WAIT_US = 1000;
constexpr uint32_t RECEIVE_WAIT_US = 3000;
constexpr uint32_t WRITE_RETRY_INTERVAL_US = 500;
constexpr uint32_t WRITE_TIMEOUT_US = 2000000;

// Auto mode runs a dialling and a listening transport side by side, and the
// SDK network stack is process-wide, so it is initialised once and shared.
// Every user is the IO owner thread, so no locking is needed.
int g_netUsers = 0;
bool g_moduleLoaded = false;
bool g_netInited = false;
bool g_netCtlInited = false;
void* g_netPool = nullptr;

void shutdownNetwork()
{
    if (g_netCtlInited) {
        sceNetCtlTerm();
        g_netCtlInited = false;
    }
    if (g_netInited) {
        sceNetTerm();
        g_netInited = false;
    }
    if (g_moduleLoaded) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        g_moduleLoaded = false;
    }
    std::free(g_netPool);
    g_netPool = nullptr;
}

// Returns 0 or a negative SDK error.
int startupNetwork()
{
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0)
        return result;
    g_moduleLoaded = true;

    g_netPool = std::malloc(NET_POOL_SIZE);
    if (g_netPool == nullptr)
        return -1;
    SceNetInitParam param{};
    param.memory = g_netPool;
    param.size = NET_POOL_SIZE;
    param.flags = 0;
    result = sceNetInit(&param);
    if (result < 0)
        return result;
    g_netInited = true;

    result = sceNetCtlInit();
    if (result < 0)
        return result;
    g_netCtlInited = true;
    return 0;
}

// Buffer sizes go on before connect or listen so the window is advertised
// from the start (an accepted socket inherits them from the listener).
void applySocketBuffers(int sock)
{
    const int receiveBuffer = RECEIVE_BUFFER_SIZE;
    const int sendBuffer = SEND_BUFFER_SIZE;
    const int rcvResult = sceNetSetsockopt(
        sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer));
    const int sndResult = sceNetSetsockopt(
        sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
    if (rcvResult < 0 || sndResult < 0)
        vitaLog("direct-wifi: socket buffer size not applied rcv=0x%08x snd=0x%08x",
                static_cast<unsigned>(rcvResult), static_cast<unsigned>(sndResult));
}

} // namespace

namespace net_stack {

int acquire()
{
    if (g_netUsers == 0) {
        const int result = startupNetwork();
        if (result < 0) {
            shutdownNetwork();
            return result;
        }
    }
    ++g_netUsers;
    return 0;
}

void release()
{
    if (g_netUsers > 0 && --g_netUsers == 0)
        shutdownNetwork();
}

} // namespace net_stack

TcpTransport::TcpTransport(const char* hostOverride, uint16_t listenPort)
    : listenPort_(listenPort)
{
    if (hostOverride != nullptr)
        std::snprintf(hostOverride_, sizeof(hostOverride_), "%s", hostOverride);
}

bool TcpTransport::initNetwork()
{
    if (holdsNet_)
        return true;
    if (g_netUsers == 0) {
        const int result = startupNetwork();
        if (result < 0) {
            shutdownNetwork();
            return fail(app::AppError::NetInitFailed, result);
        }
    }
    ++g_netUsers;
    holdsNet_ = true;
    return true;
}

bool TcpTransport::queryNetInfo(WifiNetInfo& info) const
{
    if (localNetwork_) {
        info = localNet_;
        return holdsNet_;
    }
    info = WifiNetInfo{};
    int state = 0;
    if (!holdsNet_ || sceNetCtlInetGetState(&state) < 0 ||
        state != SCE_NETCTL_STATE_CONNECTED)
        return false;

    SceNetCtlInfo net{};
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &net) < 0 ||
        net.ip_address[0] == '\0')
        return false;
    std::snprintf(info.ip, sizeof(info.ip), "%s", net.ip_address);

    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &net) >= 0)
        std::snprintf(info.ssid, sizeof(info.ssid), "%s", net.ssid);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_BSSID, &net) >= 0) {
        info.setBssid(net.bssid.data);
    }
    return true;
}

void TcpTransport::setLocalNetwork(const WifiNetInfo* info)
{
    localNetwork_ = info != nullptr && serverMode();
    localNet_ = localNetwork_ ? *info : WifiNetInfo{};
    // Look again at once rather than at the next one-second check.
    nextWifiCheckUs_ = 0;
}

app::AppError TcpTransport::lookupHost(int* detail)
{
    if (localNetwork_)
        return app::AppError::None;
    int state = 0;
    const int stateResult = sceNetCtlInetGetState(&state);
    if (stateResult < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        *detail = stateResult < 0 ? stateResult : state;
        return app::AppError::NetNotConnected;
    }

    // Listening needs the network but no phone address.
    if (serverMode())
        return app::AppError::None;

    // An explicit IP still needs the Vita to be on some network.
    if (hostOverride_[0] != '\0') {
        std::snprintf(host_, sizeof(host_), "%s", hostOverride_);
        return app::AppError::None;
    }

    SceNetCtlInfo info{};
    const int result = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE, &info);
    if (result < 0 || info.default_route[0] == '\0') {
        *detail = result;
        return app::AppError::NetNoGateway;
    }
    std::snprintf(host_, sizeof(host_), "%s", info.default_route);
    return app::AppError::None;
}

void TcpTransport::closeAttempt()
{
    if (attemptEpoll_ >= 0) {
        sceNetEpollDestroy(attemptEpoll_);
        attemptEpoll_ = -1;
    }
    if (attemptSocket_ >= 0) {
        sceNetSocketClose(attemptSocket_);
        attemptSocket_ = -1;
    }
}

void TcpTransport::closeListen()
{
    if (listenSocket_ >= 0) {
        sceNetSocketClose(listenSocket_);
        listenSocket_ = -1;
    }
}

void TcpTransport::closeSocket()
{
    closeListen();
    closeAttempt();
    if (rxEpoll_ >= 0) {
        sceNetEpollDestroy(rxEpoll_);
        rxEpoll_ = -1;
    }
    if (socket_ >= 0) {
        sceNetSocketClose(socket_);
        socket_ = -1;
    }
}

void TcpTransport::abandonAttempt(uint64_t nowUs, const char* stage, int detail)
{
    closeAttempt();
    ++attempts_;
    // Low volume: the first failure, then every fifth.
    if (attempts_ == 1 || attempts_ % 5 == 0)
        vitaLog("direct-wifi: attempt %u to %s failed stage=%s detail=0x%08x",
                attempts_, host_, stage, static_cast<unsigned>(detail));
    nextAttemptUs_ = nowUs + CONNECT_RETRY_DELAY_US;
    // The hotspot may have dropped; look again before the next attempt.
    int ignored = 0;
    wifiReady_ = lookupHost(&ignored) == app::AppError::None;
    nextWifiCheckUs_ = nowUs + WIFI_CHECK_INTERVAL_US;
}

OpenPoll TcpTransport::finishAttempt()
{
    // The connect epoll is spent; receive gets its own with the read events.
    rxEpoll_ = sceNetEpollCreate("vita_auto_rx", 0);
    if (rxEpoll_ < 0) {
        const int detail = rxEpoll_;
        rxEpoll_ = -1;
        closeAttempt();
        fail(app::AppError::NetInitFailed, detail);
        return OpenPoll::Failed;
    }
    SceNetEpollEvent rxWant{};
    rxWant.events = SCE_NET_EPOLLIN | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
    rxWant.data.fd = attemptSocket_;
    sceNetEpollControl(rxEpoll_, SCE_NET_EPOLL_CTL_ADD, attemptSocket_, &rxWant);
    socket_ = attemptSocket_;
    attemptSocket_ = -1;
    if (attemptEpoll_ >= 0) {
        sceNetEpollDestroy(attemptEpoll_);
        attemptEpoll_ = -1;
    }
    connected_ = true;
    if (serverMode())
        vitaLog("direct-wifi: phone connected to port %u", static_cast<unsigned>(listenPort_));
    else
        vitaLog("direct-wifi: connected to %s:%u after %u attempts",
                host_, static_cast<unsigned>(HEADUNIT_SERVER_PORT), attempts_ + 1);
    return OpenPoll::Connected;
}

OpenPoll TcpTransport::pollListen(uint64_t nowUs)
{
    if (listenSocket_ < 0) {
        if (nowUs < nextAttemptUs_)
            return OpenPoll::Waiting;
        const int sock = sceNetSocket(
            "vita_auto_listen", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
        if (sock < 0) {
            nextAttemptUs_ = nowUs + CONNECT_RETRY_DELAY_US;
            return OpenPoll::Waiting;
        }
        const int one = 1;
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
        applySocketBuffers(sock);
        SceNetSockaddrIn addr{};
        addr.sin_len = sizeof(addr);
        addr.sin_family = SCE_NET_AF_INET;
        addr.sin_port = sceNetHtons(listenPort_);
        addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
        int result = sceNetBind(sock, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr));
        if (result >= 0)
            result = sceNetListen(sock, 1);
        if (result < 0) {
            sceNetSocketClose(sock);
            vitaLog("direct-wifi: listen on %u failed 0x%08x",
                    static_cast<unsigned>(listenPort_), static_cast<unsigned>(result));
            nextAttemptUs_ = nowUs + CONNECT_RETRY_DELAY_US;
            return OpenPoll::Waiting;
        }
        listenSocket_ = sock;
        vitaLog("direct-wifi: listening on port %u", static_cast<unsigned>(listenPort_));
        return OpenPoll::Waiting;
    }

    SceNetSockaddrIn peer{};
    unsigned int length = sizeof(peer);
    const int accepted = sceNetAccept(
        listenSocket_, reinterpret_cast<SceNetSockaddr*>(&peer), &length);
    if (accepted == static_cast<int>(SCE_NET_ERROR_EAGAIN))
        return OpenPoll::Waiting; // nobody has connected yet
    if (accepted < 0) {
        // The listener is broken (network reset, socket invalidated): drop it
        // and open a fresh one after the usual retry delay.
        vitaLog("direct-wifi: accept failed 0x%08x, listening again",
                static_cast<unsigned>(accepted));
        closeListen();
        nextAttemptUs_ = nowUs + CONNECT_RETRY_DELAY_US;
        return OpenPoll::Waiting;
    }
    const int one = 1;
    sceNetSetsockopt(accepted, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    sceNetSetsockopt(accepted, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));
    closeListen();
    attemptSocket_ = accepted;
    return finishAttempt();
}

OpenPoll TcpTransport::startAttempt(uint64_t nowUs)
{
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(HEADUNIT_SERVER_PORT);
    if (sceNetInetPton(SCE_NET_AF_INET, host_, &addr.sin_addr) != 1) {
        fail(app::AppError::NetNoGateway, -2);
        return OpenPoll::Failed;
    }

    const int sock = sceNetSocket("vita_auto_aa", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) {
        abandonAttempt(nowUs, "socket", sock);
        return OpenPoll::Waiting;
    }
    const int one = 1;
    applySocketBuffers(sock);
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    sceNetSetsockopt(sock, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));
    attemptSocket_ = sock;

    const int connectResult = sceNetConnect(
        sock, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr));
    if (connectResult >= 0)
        return finishAttempt();
    if (connectResult != static_cast<int>(SCE_NET_ERROR_EINPROGRESS)) {
        abandonAttempt(nowUs, "connect", connectResult);
        return OpenPoll::Waiting;
    }

    attemptEpoll_ = sceNetEpollCreate("vita_auto_connect", 0);
    if (attemptEpoll_ < 0) {
        const int detail = attemptEpoll_;
        attemptEpoll_ = -1;
        abandonAttempt(nowUs, "epoll", detail);
        return OpenPoll::Waiting;
    }
    SceNetEpollEvent want{};
    want.events = SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
    want.data.fd = sock;
    sceNetEpollControl(attemptEpoll_, SCE_NET_EPOLL_CTL_ADD, sock, &want);
    attemptDeadlineUs_ = nowUs + CONNECT_ATTEMPT_US;
    return OpenPoll::Waiting;
}

bool TcpTransport::beginOpen(bool (*cancelled)(), bool strict)
{
    clearError();
    sentBytes_ = 0;
    receivedBytes_ = 0;
    lastDataUs_ = 0;
    lastCallUs_ = 0;
    nextGapLogUs_ = 0;
    rxGaps_ = 0;
    loopGaps_ = 0;
    maxRxGapUs_ = 0;
    maxLoopGapUs_ = 0;
    attempts_ = 0;
    wifiReady_ = false;
    nextAttemptUs_ = 0;
    nextWifiCheckUs_ = 0;
    if (cancelled())
        return false;

    vitaLog("direct-wifi: opening (%s port %u)", serverMode() ? "listen" : "connect",
            static_cast<unsigned>(serverMode() ? listenPort_ : HEADUNIT_SERVER_PORT));
    if (!initNetwork())
        return false;

    int detail = 0;
    const app::AppError lookup = lookupHost(&detail);
    if (lookup == app::AppError::None) {
        wifiReady_ = true;
        vitaLog("direct-wifi: phone host %s", host_);
    } else if (strict) {
        return fail(lookup, detail);
    } else {
        vitaLog("direct-wifi: not on Wi-Fi yet; USB can still connect");
    }
    return true;
}

OpenPoll TcpTransport::pollOpen(bool (*cancelled)())
{
    if (cancelled())
        return OpenPoll::Failed;
    const uint64_t nowUs = sceKernelGetSystemTimeWide();

    if (!wifiReady_) {
        if (nowUs < nextWifiCheckUs_)
            return OpenPoll::Waiting;
        nextWifiCheckUs_ = nowUs + WIFI_CHECK_INTERVAL_US;
        int detail = 0;
        if (lookupHost(&detail) != app::AppError::None)
            return OpenPoll::Waiting;
        wifiReady_ = true;
        vitaLog("direct-wifi: phone host %s", host_);
    }

    if (serverMode())
        return pollListen(nowUs);

    if (attemptSocket_ < 0) {
        if (nowUs < nextAttemptUs_)
            return OpenPoll::Waiting;
        return startAttempt(nowUs);
    }

    SceNetEpollEvent got{};
    if (sceNetEpollWait(attemptEpoll_, &got, 1, CONNECT_POLL_WAIT_US) > 0) {
        int soError = -1;
        unsigned int length = sizeof(soError);
        sceNetGetsockopt(attemptSocket_, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &soError, &length);
        if (soError == 0)
            return finishAttempt();
        abandonAttempt(nowUs, "so_error", soError);
    } else if (nowUs >= attemptDeadlineUs_) {
        abandonAttempt(nowUs, "timeout", 0);
    }
    return OpenPoll::Waiting;
}

bool TcpTransport::open(bool (*cancelled)())
{
    if (!beginOpen(cancelled, true))
        return false;
    // Like USB, waiting for the phone has no time limit; cancellation ends it.
    for (;;) {
        const OpenPoll poll = pollOpen(cancelled);
        if (poll == OpenPoll::Connected)
            return true;
        if (poll == OpenPoll::Failed)
            return false;
        sceKernelDelayThread(POLL_INTERVAL_US);
    }
}

int TcpTransport::close()
{
    connected_ = false;
    closeSocket();
    if (holdsNet_) {
        holdsNet_ = false;
        if (--g_netUsers == 0)
            shutdownNetwork();
    }
    return 0;
}

bool TcpTransport::send(const uint8_t* data, size_t size)
{
    if (!connected_ || data == nullptr || size == 0 || size > MAX_TRANSFER_SIZE)
        return fail(app::AppError::NetSendRejected);

    size_t offset = 0;
    uint32_t waitedUs = 0;
    while (offset < size) {
        const int written = sceNetSend(socket_, data + offset, size - offset, 0);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            sentBytes_ += static_cast<uint32_t>(written);
            waitedUs = 0;
            continue;
        }
        if (written != static_cast<int>(SCE_NET_ERROR_EAGAIN)) {
            connected_ = false;
            return fail(app::AppError::NetWriteFailed, written);
        }
        if (waitedUs >= WRITE_TIMEOUT_US) {
            connected_ = false;
            return fail(app::AppError::NetWriteTimedOut);
        }
        sceKernelDelayThread(WRITE_RETRY_INTERVAL_US);
        waitedUs += WRITE_RETRY_INTERVAL_US;
    }
    return true;
}

int TcpTransport::receive(std::vector<uint8_t>& data, UsbRxTiming* timing)
{
    data.clear();
    if (timing != nullptr)
        *timing = UsbRxTiming{};

    if (!connected_) {
        fail(app::AppError::NetReadFailed);
        return -1;
    }

    const uint64_t callUs = sceKernelGetSystemTimeWide();
    if (lastCallUs_ != 0 && callUs - lastCallUs_ > maxLoopGapUs_)
        maxLoopGapUs_ = callUs - lastCallUs_;
    if (lastCallUs_ != 0 && callUs - lastCallUs_ >= LOOP_GAP_LOG_US) {
        ++loopGaps_;
        if (callUs >= nextGapLogUs_) {
            vitaLog("direct-wifi: session loop gap %u ms (slow Vita side)",
                    static_cast<unsigned>((callUs - lastCallUs_) / 1000));
            nextGapLogUs_ = callUs + GAP_LOG_INTERVAL_US;
        }
    }
    lastCallUs_ = callUs;

    uint8_t buffer[MAX_TRANSFER_SIZE];
    int received = sceNetRecv(socket_, buffer, sizeof(buffer), 0);
    if (received == static_cast<int>(SCE_NET_ERROR_EAGAIN)) {
        SceNetEpollEvent got{};
        // Doubles as the idle sleep; a timeout is not an error.
        if (sceNetEpollWait(rxEpoll_, &got, 1, RECEIVE_WAIT_US) <= 0)
            return 0;
        received = sceNetRecv(socket_, buffer, sizeof(buffer), 0);
        if (received == static_cast<int>(SCE_NET_ERROR_EAGAIN))
            return 0;
    }

    if (received == 0) {
        connected_ = false;
        fail(app::AppError::NetPeerClosed);
        return -1;
    }
    if (received < 0) {
        connected_ = false;
        fail(app::AppError::NetReadFailed, received);
        return -1;
    }


    data.assign(buffer, buffer + received);
    while (received == static_cast<int>(sizeof(buffer)) && data.size() < RECEIVE_DRAIN_LIMIT) {
        received = sceNetRecv(socket_, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break; 
        data.insert(data.end(), buffer, buffer + received);
    }

    const uint64_t nowUs = sceKernelGetSystemTimeWide();
    if (lastDataUs_ != 0) {
        const uint64_t gapUs = nowUs - lastDataUs_;
        if (gapUs > maxRxGapUs_)
            maxRxGapUs_ = gapUs;
        if (gapUs >= RX_GAP_LOG_US) {
            ++rxGaps_;
            if (nowUs >= nextGapLogUs_) {
                vitaLog("direct-wifi: no data for %u ms, then %u bytes (network)",
                        static_cast<unsigned>(gapUs / 1000),
                        static_cast<unsigned>(data.size()));
                nextGapLogUs_ = nowUs + GAP_LOG_INTERVAL_US;
            }
        }
    }
    lastDataUs_ = nowUs;

    if (timing != nullptr) {
        timing->firstCompletedAtUs = nowUs;
        timing->lastCompletedAtUs = nowUs;
    }
    receivedBytes_ += static_cast<uint32_t>(data.size());
    return 1;
}

void TcpTransport::logDiagnostics(const char* reason) const
{
    vitaLog("direct-wifi: diag reason=%s connected=%d tx_bytes=%u rx_bytes=%u "
            "rx_gaps=%u max_rx_gap_ms=%u loop_gaps=%u max_loop_gap_ms=%u",
            reason != nullptr ? reason : "unknown", connected_ ? 1 : 0,
            sentBytes_, receivedBytes_, rxGaps_,
            static_cast<unsigned>(maxRxGapUs_ / 1000), loopGaps_,
            static_cast<unsigned>(maxLoopGapUs_ / 1000));
}
