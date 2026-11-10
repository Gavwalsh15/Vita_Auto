// User-mode SUPRX bridge.
#include "vita_auto_usb.h"
#include "kernel_imports.h"

#include <psp2/kernel/modulemgr.h>
#include <psp2/net/net_syscalls.h>

int module_start(SceSize argc, const void *args);
int _start(SceSize argc, const void *args)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}

int vitaAutoUsbClientLoad(const char *suprxPath)
{
    // The executable links loader.c to load this module; inside the bridge the exported entry point is a no-op.
    (void)suprxPath;
    return 0;
}

int vitaAutoUsbGetState(void)
{
    return ksceVitaAutoUsbGetState();
}

int vitaAutoUsbReadWaitTimed(
    void *buffer,
    uint32_t capacity,
    uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *metadata
)
{
    return ksceVitaAutoUsbReadWaitTimed(
        buffer,
        capacity,
        timeoutUs,
        metadata
    );
}

int vitaAutoUsbWake(void)
{
    return ksceVitaAutoUsbWake();
}

int vitaAutoUsbWrite(const void *buffer, uint32_t length)
{
    return ksceVitaAutoUsbWrite(buffer, length);
}

int vitaAutoUsbGetAccessoryActivateResult(void)
{
    return ksceVitaAutoUsbGetAccessoryActivateResult();
}

int vitaAutoUsbGetHostModeResult(void)
{
    return ksceVitaAutoUsbGetHostModeResult();
}

int vitaAutoUsbGetDriverRegisterResult(void)
{
    return ksceVitaAutoUsbGetDriverRegisterResult();
}

unsigned int vitaAutoUsbGetLastVidPid(void)
{
    return ksceVitaAutoUsbGetLastVidPid();
}

int vitaAutoUsbGetLastFailureStage(void)
{
    return ksceVitaAutoUsbGetLastFailureStage();
}

int vitaAutoUsbGetLastFailureResult(void)
{
    return ksceVitaAutoUsbGetLastFailureResult();
}

int vitaAutoUsbGetLastFailureCount(void)
{
    return ksceVitaAutoUsbGetLastFailureCount();
}

int vitaAutoUsbStart(void)
{
    return ksceVitaAutoUsbStart();
}

int vitaAutoUsbStop(void)
{
    return ksceVitaAutoUsbStop();
}

int vitaAutoUsbDrainReads(void)
{
    return ksceVitaAutoUsbDrainReads();
}

int vitaAutoUsbGetCurrentHostMode(void)
{
    return ksceVitaAutoUsbGetCurrentHostMode();
}

int vitaAutoUsbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *diagnostics)
{
    return ksceVitaAutoUsbGetRxDiagnostics(diagnostics);
}

int vitaAutoBtStart(void)
{
    return ksceVitaAutoBtStart();
}

int vitaAutoBtStop(void)
{
    return ksceVitaAutoBtStop();
}

int vitaAutoBtReadWaitTimed(void *buffer, uint32_t capacity, uint32_t timeoutUs)
{
    return ksceVitaAutoBtReadWaitTimed(buffer, capacity, timeoutUs);
}

int vitaAutoBtWrite(const void *buffer, uint32_t length)
{
    return ksceVitaAutoBtWrite(buffer, length);
}

int vitaAutoBtGetInfo(VitaAutoBtInfo *info)
{
    return ksceVitaAutoBtGetInfo(info);
}

int vitaAutoBtEnable(void)
{
    return ksceVitaAutoBtEnable();
}

int vitaAutoBtDisable(void)
{
    return ksceVitaAutoBtDisable();
}

int vitaAutoBtReadEvents(VitaAutoBtEvent *events, uint32_t maxEvents)
{
    return ksceVitaAutoBtReadEvents(events, maxEvents);
}

int vitaAutoBtConfigureName(const char *wanted, const char *recoverOriginal)
{
    return ksceVitaAutoBtConfigureName(wanted, recoverOriginal);
}

int vitaAutoBtGetOriginalName(char *buffer, uint32_t size)
{
    return ksceVitaAutoBtGetOriginalName(buffer, size);
}

int vitaAutoBtDisconnect(uint32_t mac0, uint32_t mac1)
{
    return ksceVitaAutoBtDisconnect(mac0, mac1);
}

int vitaAutoBtConnectState(uint32_t mac0, uint32_t mac1)
{
    return ksceVitaAutoBtConnectState(mac0, mac1);
}

int vitaAutoBtSetOptions(uint32_t flags)
{
    return ksceVitaAutoBtSetOptions(flags);
}

int vitaAutoApStart(const VitaAutoApConfig *config)
{
    return ksceVitaAutoApStart(config);
}

static void apDeconfigureInterface(void);

int vitaAutoApStop(void)
{
    const int result = ksceVitaAutoApStop();
    // AP is down: undo ConfigureInterface on wlan0 so the system finds the interface as it was.
    apDeconfigureInterface();
    return result;
}

int vitaAutoApKeepAlive(void)
{
    return ksceVitaAutoApKeepAlive();
}

int vitaAutoApGetInfo(VitaAutoApInfo *info)
{
    return ksceVitaAutoApGetInfo(info);
}

// FreeBSD interface ioctls (SceNetPs syscalls); the descriptor must come from sceNetSyscallSocket.
#define AP_SIOCSIFADDR    0x8020690cu
#define AP_SIOCSIFFLAGS   0x80206910u
#define AP_SIOCGIFFLAGS   0xc0206911u
#define AP_SIOCSIFNETMASK 0x80206916u
#define AP_SIOCDIFADDR    0x80206919u
#define AP_IFF_UP         0x1

typedef struct ApIfreq {
    char name[16];
    union {
        struct {
            uint8_t len;
            uint8_t family;
            uint16_t port;
            uint32_t addr; // Network byte order
            uint8_t zero[8];
        } in;
        int16_t flags;
    } u;
} ApIfreq;

static void apIfreqAddress(ApIfreq *req, uint32_t hostOrder)
{
    for (int i = 0; i < (int)sizeof(*req); i++)
        ((uint8_t *)req)[i] = 0;
    req->name[0] = 'w'; req->name[1] = 'l'; req->name[2] = 'a';
    req->name[3] = 'n'; req->name[4] = '0';
    req->u.in.len = 16;
    req->u.in.family = 2; // AF_INET
    req->u.in.addr = __builtin_bswap32(hostOrder);
}

// What wlan0 looked like before ConfigureInterface, for the undo.
static int g_apInterfaceChanged;
static int16_t g_apOriginalFlags;

int vitaAutoApConfigureInterface(void)
{
    ApIfreq req;
    int result;
    const int sock = sceNetSyscallSocket("vita_auto_ifcfg", 2, 2, 0); // AF_INET, DGRAM
    if (sock < 0)
        return sock;
    apIfreqAddress(&req, VITA_AUTO_AP_IP);
    result = sceNetSyscallIoctl(sock, AP_SIOCSIFADDR, &req);
    if (result >= 0) {
        apIfreqAddress(&req, VITA_AUTO_AP_NETMASK);
        result = sceNetSyscallIoctl(sock, AP_SIOCSIFNETMASK, &req);
    }
    if (result >= 0) {
        apIfreqAddress(&req, 0);
        if (sceNetSyscallIoctl(sock, AP_SIOCGIFFLAGS, &req) < 0)
            req.u.flags = 0;
        if (!g_apInterfaceChanged)
            g_apOriginalFlags = req.u.flags;
        g_apInterfaceChanged = 1;
        req.u.flags |= AP_IFF_UP;
        result = sceNetSyscallIoctl(sock, AP_SIOCSIFFLAGS, &req);
    }
    if (result >= 0) {
        apIfreqAddress(&req, 0);
        result = sceNetSyscallIoctl(sock, AP_SIOCGIFFLAGS, &req);
        if (result >= 0)
            result = (uint16_t)req.u.flags;
    }
    sceNetSyscallClose(sock);
    return result;
}

// Removes the AP address from wlan0 and restores its flags; harmless if never configured.
static void apDeconfigureInterface(void)
{
    ApIfreq req;
    int sock;
    if (!g_apInterfaceChanged)
        return;
    g_apInterfaceChanged = 0;
    sock = sceNetSyscallSocket("vita_auto_ifcfg", 2, 2, 0);
    if (sock < 0)
        return;
    apIfreqAddress(&req, VITA_AUTO_AP_IP);
    sceNetSyscallIoctl(sock, AP_SIOCDIFADDR, &req);
    apIfreqAddress(&req, 0);
    if (sceNetSyscallIoctl(sock, AP_SIOCGIFFLAGS, &req) >= 0) {
        req.u.flags = (int16_t)((req.u.flags & ~AP_IFF_UP) | (g_apOriginalFlags & AP_IFF_UP));
        sceNetSyscallIoctl(sock, AP_SIOCSIFFLAGS, &req);
    }
    sceNetSyscallClose(sock);
}
