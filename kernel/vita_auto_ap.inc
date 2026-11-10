// Vita-hosted Wi-Fi access point (uAP) for firmware 3.65. SceWlanBt only drives the chip's AP engine as a
// station, so AP host commands go through the driver's own command path (cmdbuf[9] bss byte rewritten after enqueue).
// Sending also needs two hooks: the SceNetPs output callback (AP_START_AT) frees frames unless a station is
// connected, and the TX worker (0x235c) builds TxPD with bss_type station, rewritten to uAP in the SDIO write hooks.
// Drop the AP before the app exits (panic otherwise); a watchdog does it if KeepAlive stops.

#define AP_ALLOC_AT          0x2954 // (adapter, size) -> command node
#define AP_ENQUEUE_AT        0x2db4 // (adapter, node, opcode, body length)
#define AP_RESPONSE_AT       0x1cd4 // (adapter, message, ...): responses and events
#define AP_NODE_CMDBUF       4      // node -> command buffer (SDIO header first)
#define AP_NODE_PAYLOAD      8      // node -> body, cmdbuf + 0xc
#define AP_CMDBUF_BSS        9      // high byte of seq_num
#define AP_BSS_UAP           0x10   // bss_num 0, bss_type 1 (uAP)
#define AP_STAMP_SPINS       2000   // Rewrites that outrun the send worker
// Driver command bookkeeping (in the adapter): the node in flight and a pool of eight nodes (free while its buffer pointer is 0).
#define AP_ADAPTER_PENDING   0xa8
#define AP_ADAPTER_POOL      0xb4
#define AP_POOL_STRIDE       0x38
#define AP_POOL_SLOTS        8
#define AP_ALLOC_TRIES       20     // x AP_POLL_US / 10 while the pool is full

// Data path: the driver context is SceWlanBt seg1 + 0x240; SceNetPs' driver struct is at context + 0x630 (+0x6c points back).
#define AP_MODULE_NID        0x94fa8830 // SceWlanBt of firmware 3.65
#define AP_START_AT          0x7348 // SceNetPs output callback (driver struct)
#define AP_WLAN_CTX_AT       0x240  // Driver context, in seg1
#define AP_DRV_CTX           0x6c   // Driver struct -> context
#define AP_CTX_EVENT         0x6e4  // Worker event flag
#define AP_CTX_FLAGS         0x704
#define AP_CTX_TX_PENDING    0x70c  // The TX worker re-runs while this is above 1
#define AP_CTX_GUARD         0x718  // Use count; acquire fails once the driver is closing
#define AP_GUARD_ACQUIRE_AT  0xe50  // (guard) -> 0, or the error once closed
#define AP_GUARD_RELEASE_AT  0xe70
#define AP_FLAGS_NO_TX       0x004c0030 // Resetting/suspended (0x400020), or the TX worker defers (0x80010)
#define AP_EVENT_TX          0x80
#define AP_SDIF_LIB_NID      0x96d306fa // SceSdifForDriver
#define AP_SDIF_WRITE_NID    0x3c4cdc8b // (handle, address, length, buffer, flags)
#define AP_SDIF_WRITE_ASYNC_NID 0x855c95e1 // (handle, request, ...): +8 length, +0xc buffer
#define AP_SDIO_TYPE_DATA    0
#define AP_TXPD_SIZE         0x10   // tx_pkt_offset the driver writes
#define AP_TX_HEADER         (4 + AP_TXPD_SIZE) // SDIO header + TxPD
#define AP_TXPD_BSS_TYPE     4      // SDIO header, then TxPD.bss_type
#define AP_BSS_TYPE_UAP      1

#define AP_CMD_SYS_CONFIG    0x00b0
#define AP_CMD_BSS_START     0x00b1
#define AP_CMD_BSS_STOP      0x00b2
#define AP_CMD_RESPONSE      0x8000
#define AP_ACTION_SET        0x0001

#define AP_TLV_SSID          0x0000
#define AP_TLV_CHANNEL_BAND  0x012a
#define AP_TLV_MAC_ADDRESS   0x012b
#define AP_TLV_BCAST_SSID    0x0130
#define AP_TLV_PASSPHRASE    0x013c
#define AP_TLV_ENCRY_PROTO   0x0140
#define AP_TLV_AKMP          0x0141
#define AP_TLV_PWK_CIPHER    0x0191
#define AP_TLV_GWK_CIPHER    0x0192
#define AP_PROTOCOL_OPEN     0x0001
#define AP_PROTOCOL_WPA2     0x0020
#define AP_KEY_MGMT_PSK      0x0002
#define AP_CIPHER_CCMP       0x08

#define AP_MSG_TYPE_EVENT    3
#define AP_EVENT_STA_DEAUTH  0x2c
#define AP_EVENT_STA_ASSOC   0x2d
#define AP_EVENT_MAC_AT      10 // SDIO header, event cause, 2 reserved bytes

#define AP_ADAPTER_WAIT_US   60000000
#define AP_SETTLE_US         10000000
#define AP_REPLY_WAIT_US     3000000
#define AP_POLL_US           100000
#define AP_RING_SIZE         32


typedef int (*ApGuardFn)(void *guard);
typedef void *(*ApAllocFn)(void *adapter, int size);
typedef int (*ApEnqueueFn)(void *adapter, void *node, int opcode, int length);

typedef struct ApMessage {
    uint16_t type;
    uint16_t code;
    uint16_t result;
    uint8_t mac[6];
} ApMessage;

static VitaAutoApConfig g_apConfig;
static volatile int g_apState = VITA_AUTO_AP_OFF;
static volatile int g_apFailureStage;
static volatile int g_apFailureResult;
static volatile int g_apConfigVariant = -1;
static volatile uint32_t g_apAdapterWaitMs;
static volatile uint32_t g_apModuleNid;
static volatile int g_apLifecycleBusy;
static volatile int g_apExit;
static volatile uint64_t g_apLastPingUs;
static SceUID g_apWorker = -1;

static ApAllocFn g_apAlloc;
static ApEnqueueFn g_apEnqueue;
// Kept across starts: once the station goes quiet the driver issues no command for the hooks to catch. SceWlanBt never unloads.
static void *volatile g_apAdapter;
static tai_hook_ref_t g_apEnqueueRef;
static tai_hook_ref_t g_apResponseRef;
static SceUID g_apEnqueueHook = -1;
static SceUID g_apResponseHook = -1;
static volatile uint32_t g_apCommandsSeen;
static volatile int32_t g_apCommandFailure; // 0, -1 no reply, -2 pool full, -3 node has no buffer
static volatile uint32_t g_apPoolMask;      // Pool slots in use when a command last failed
static volatile uint32_t g_apPendingAtFail; // Node in flight at that moment
static volatile int32_t g_apStopResult = -100;
static volatile uint32_t g_apStopPending;   // Still in flight after BSS_STOP

static uint8_t *g_apWlanCtx;
static uintptr_t g_apWlanSeg0;
static tai_hook_ref_t g_apStartRef;
static tai_hook_ref_t g_apSdifWriteRef;
static tai_hook_ref_t g_apSdifWriteAsyncRef;
static SceUID g_apStartHook = -1;
static SceUID g_apSdifWriteHook = -1;
static SceUID g_apSdifWriteAsyncHook = -1;
static volatile uint32_t g_apTxKicks;
static volatile uint32_t g_apTxFrames;
static volatile uint32_t g_apTxStamped;

// Written by the response hook (driver context), read by the worker.
static ApMessage g_apRing[AP_RING_SIZE];
static volatile uint32_t g_apRingHead;
static uint32_t g_apRingTail;
static volatile uint32_t g_apResponses;
static volatile uint32_t g_apAssociations;
static volatile uint32_t g_apDeauths;
static volatile uint16_t g_apLastEvent;
static uint8_t g_apLastStation[6];

static void apFail(int stage, int result)
{
    g_apFailureStage = stage;
    g_apFailureResult = result;
    g_apState = VITA_AUTO_AP_FAILED;
    ksceDebugPrintf("[VitaAutoAP] Failed stage=%d result=0x%08x\n", stage, result);
}

static int apStartHook(int drv, int a1, int a2, int a3)
{
    if (g_apState == VITA_AUTO_AP_UP && drv != 0 && g_apWlanSeg0 != 0) {
        uint8_t *ctx = *(uint8_t **)(drv + AP_DRV_CTX);
        if (ctx != NULL) {
            uint8_t *guard = ctx + AP_CTX_GUARD;
            if (((ApGuardFn)((g_apWlanSeg0 + AP_GUARD_ACQUIRE_AT) | 1))(guard) >= 0) {
                const uint32_t flags = *(volatile uint32_t *)(ctx + AP_CTX_FLAGS);
                if ((flags & AP_FLAGS_NO_TX) == 0) {
                    ++g_apTxKicks;
                    if (__sync_fetch_and_add(
                            (volatile uint32_t *)(ctx + AP_CTX_TX_PENDING), 1) == 0)
                        ksceKernelSetEventFlag(*(SceUID *)(ctx + AP_CTX_EVENT), AP_EVENT_TX);
                    ((ApGuardFn)((g_apWlanSeg0 + AP_GUARD_RELEASE_AT) | 1))(guard);
                    return 0;
                }
                ((ApGuardFn)((g_apWlanSeg0 + AP_GUARD_RELEASE_AT) | 1))(guard);
            }
        }
    }
    return vitaAutoHookContinue(g_apStartRef, drv, a1, a2, a3, 0, 0);
}

// Marks a data frame for the AP. Commands, firmware download and register writes don't match the TX worker's header, so they pass.
static void apStampTx(uint8_t *buf, uint32_t length)
{
    uint32_t total, packet, offset;
    if (buf == NULL || length < AP_TX_HEADER)
        return;
    if (buf[2] != AP_SDIO_TYPE_DATA || buf[3] != 0)
        return;
    total = buf[0] | ((uint32_t)buf[1] << 8);
    packet = buf[6] | ((uint32_t)buf[7] << 8);
    offset = buf[8] | ((uint32_t)buf[9] << 8);
    if (offset != AP_TXPD_SIZE || total != packet + AP_TX_HEADER || total > length)
        return;
    ++g_apTxFrames;
    if (buf[AP_TXPD_BSS_TYPE] == 0) {
        buf[AP_TXPD_BSS_TYPE] = AP_BSS_TYPE_UAP;
        ++g_apTxStamped;
    }
}

// The last frame of each batch, and every command.
static int apSdifWriteHook(int handle, int address, int length, int buffer, int flags)
{
    if (g_apState == VITA_AUTO_AP_UP)
        apStampTx((uint8_t *)buffer, (uint32_t)length);
    return vitaAutoHookContinue(g_apSdifWriteRef, handle, address, length, buffer, flags, 0);
}

// The other frames of a batch; only the TX worker uses this one.
static int apSdifWriteAsyncHook(int handle, int request, int a2, int a3)
{
    if (g_apState == VITA_AUTO_AP_UP && request != 0) {
        const uint8_t *r = (const uint8_t *)request;
        apStampTx(*(uint8_t *const *)(r + 0xc), *(const uint32_t *)(r + 8));
    }
    return vitaAutoHookContinue(g_apSdifWriteAsyncRef, handle, request, a2, a3, 0, 0);
}

// Any driver command hands us the adapter the command path needs.
static int apEnqueueHook(int adapter, int node, int opcode, int length)
{
    if (g_apAdapter == NULL)
        g_apAdapter = (void *)adapter;
    ++g_apCommandsSeen;
    return vitaAutoHookContinue(g_apEnqueueRef, adapter, node, opcode, length, 0, 0);
}

static int apResponseHook(int adapter, int message, int length, int a3)
{
    const uint8_t *msg = (const uint8_t *)message;
    if (g_apAdapter == NULL)
        g_apAdapter = (void *)adapter;
    if (msg != NULL) {
        const uint32_t head = g_apRingHead;
        ApMessage *slot = &g_apRing[head % AP_RING_SIZE];
        slot->type = (uint16_t)(msg[2] | (msg[3] << 8));
        slot->code = (uint16_t)(msg[4] | (msg[5] << 8));
        slot->result = (uint16_t)(msg[0xa] | (msg[0xb] << 8));
        if (slot->type == AP_MSG_TYPE_EVENT && (unsigned int)length >= AP_EVENT_MAC_AT + 6)
            memcpy(slot->mac, msg + AP_EVENT_MAC_AT, 6);
        else
            memset(slot->mac, 0, 6);
        __sync_synchronize();
        g_apRingHead = head + 1;
        ++g_apResponses;
    }
    return vitaAutoHookContinue(g_apResponseRef, adapter, message, length, a3, 0, 0);
}

static int apDrain(uint16_t want)
{
    int result = -1;
    while (g_apRingTail != g_apRingHead) {
        const ApMessage *m = &g_apRing[g_apRingTail % AP_RING_SIZE];
        if (want != 0 && m->code == want) {
            result = m->result;
        } else if (m->type == AP_MSG_TYPE_EVENT && !(m->code & AP_CMD_RESPONSE)) {
            g_apLastEvent = m->code;
            if (m->code == AP_EVENT_STA_ASSOC) {
                memcpy(g_apLastStation, m->mac, 6);
                ++g_apAssociations;
            } else if (m->code == AP_EVENT_STA_DEAUTH) {
                ++g_apDeauths;
            }
        }
        ++g_apRingTail;
    }
    return result;
}

static int apPingExpired(void)
{
    return ksceKernelGetSystemTimeWide() - g_apLastPingUs >
           (uint64_t)VITA_AUTO_AP_KEEPALIVE_MS * 1000;
}

static int apShouldStop(void)
{
    if (g_apExit)
        return 1;
    if (apPingExpired()) {
        apFail(VITA_AUTO_AP_FAILURE_WATCHDOG, 0);
        return 1;
    }
    return 0;
}

static uint8_t *apPutTlv(uint8_t *body, int *offset, unsigned int type, unsigned int length)
{
    uint8_t *tlv = body + *offset;
    tlv[0] = (uint8_t)type;
    tlv[1] = (uint8_t)(type >> 8);
    tlv[2] = (uint8_t)length;
    tlv[3] = (uint8_t)(length >> 8);
    *offset += 4 + (int)length;
    return tlv + 4;
}

static uint32_t apPendingCommand(void)
{
    return *(volatile uint32_t *)((uint8_t *)g_apAdapter + AP_ADAPTER_PENDING);
}

static void apSnapshotPool(void)
{
    uint32_t mask = 0;
    for (int i = 0; i < AP_POOL_SLOTS; i++) {
        if (*(volatile uint32_t *)((uint8_t *)g_apAdapter + AP_ADAPTER_POOL + i * AP_POOL_STRIDE) != 0)
            mask |= 1u << i;
    }
    g_apPoolMask = mask;
    g_apPendingAtFail = apPendingCommand();
}

// Sends one uAP command and waits. Returns the firmware status (0 ok) or -1 no reply, -2 pool full, -3 no buffers.
static int apCommand(int opcode, const uint8_t *body, int length)
{
    uint8_t *node = NULL;
    uint8_t *cmdbuf;
    uint8_t *payload;
    int waitedUs;

    // A node may be taken by the driver's own command for a moment.
    for (int tries = 0; tries < AP_ALLOC_TRIES; tries++) {
        node = (uint8_t *)g_apAlloc(g_apAdapter, length > 8 ? length : 8);
        if (node != NULL || g_apExit)
            break;
        ksceKernelDelayThread(AP_POLL_US / 10);
    }
    if (node == NULL) {
        g_apCommandFailure = -2;
        apSnapshotPool();
        return -2;
    }
    cmdbuf = *(uint8_t **)(node + AP_NODE_CMDBUF);
    payload = *(uint8_t **)(node + AP_NODE_PAYLOAD);
    if (cmdbuf == NULL || payload == NULL) {
        g_apCommandFailure = -3;
        apSnapshotPool();
        return -3;
    }
    if (length > 0)
        memcpy(payload, body, length);
    apDrain(0);
    g_apEnqueue(g_apAdapter, node, opcode, length);
    for (int i = 0; i < AP_STAMP_SPINS; i++)
        ((volatile uint8_t *)cmdbuf)[AP_CMDBUF_BSS] = AP_BSS_UAP;

    for (waitedUs = 0; waitedUs < AP_REPLY_WAIT_US; waitedUs += AP_POLL_US / 10) {
        const int result = apDrain((uint16_t)(opcode | AP_CMD_RESPONSE));
        if (result >= 0)
            return result;
        ksceKernelDelayThread(AP_POLL_US / 10);
    }
    g_apCommandFailure = -1;
    apSnapshotPool();
    return -1;
}

// variant 0: everything; 1: SSID and security only (what the firmware must take).
static int apBuildConfig(uint8_t *body, int variant)
{
    const VitaAutoApConfig *c = &g_apConfig;
    const unsigned int ssidLength = strnlen(c->ssid, 32);
    const unsigned int passLength = strnlen(c->passphrase, 63);
    int offset = 0;
    uint8_t *v;

    body[offset++] = (uint8_t)AP_ACTION_SET;
    body[offset++] = (uint8_t)(AP_ACTION_SET >> 8);
    if (variant == 0) {
        v = apPutTlv(body, &offset, AP_TLV_MAC_ADDRESS, 6);
        memcpy(v, c->mac, 6);
    }
    v = apPutTlv(body, &offset, AP_TLV_SSID, ssidLength);
    memcpy(v, c->ssid, ssidLength);
    if (variant == 0) {
        v = apPutTlv(body, &offset, AP_TLV_BCAST_SSID, 1);
        v[0] = 1;
        v = apPutTlv(body, &offset, AP_TLV_CHANNEL_BAND, 2);
        v[0] = 0; // 2.4 GHz, 20 MHz
        v[1] = c->channel;
    }
    v = apPutTlv(body, &offset, AP_TLV_ENCRY_PROTO, 2);
    v[0] = (uint8_t)(passLength != 0 ? AP_PROTOCOL_WPA2 : AP_PROTOCOL_OPEN);
    v[1] = 0;
    if (passLength != 0) {
        v = apPutTlv(body, &offset, AP_TLV_AKMP, 4);
        v[0] = AP_KEY_MGMT_PSK; v[1] = 0;
        v[2] = 0; v[3] = 0; // The firmware runs the 4-way handshake itself
        v = apPutTlv(body, &offset, AP_TLV_PWK_CIPHER, 4);
        v[0] = (uint8_t)AP_PROTOCOL_WPA2; v[1] = 0;
        v[2] = AP_CIPHER_CCMP; v[3] = 0;
        v = apPutTlv(body, &offset, AP_TLV_GWK_CIPHER, 2);
        v[0] = AP_CIPHER_CCMP; v[1] = 0;
        v = apPutTlv(body, &offset, AP_TLV_PASSPHRASE, passLength);
        memcpy(v, c->passphrase, passLength);
    }
    return offset;
}

// A function entry is a push (B4xx/B5xx or E92D); anything else means the offsets aren't this build's: refuse rather than jump.
static int apLooksLikeEntry(uintptr_t address)
{
    const uint16_t h = *(const volatile uint16_t *)address;
    return (h & 0xfe00) == 0xb400 || h == 0xe92d;
}

static int apFindWlan(SceUID *modid, uintptr_t *seg0, uintptr_t *seg1)
{
    tai_module_info_t tinfo;
    SceKernelModuleInfo info;
    tinfo.size = sizeof(tinfo);
    if (taiGetModuleInfoForKernel(KERNEL_PID, "SceWlanBt", &tinfo) < 0)
        return -1;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (ksceKernelGetModuleInfo(KERNEL_PID, tinfo.modid, &info) < 0)
        return -1;
    g_apModuleNid = tinfo.module_nid;
    *modid = tinfo.modid;
    *seg0 = (uintptr_t)info.segments[0].vaddr;
    *seg1 = (uintptr_t)info.segments[1].vaddr;
    return 0;
}

// The output callback is patched around, so check its whole head: push, context from +0x6c, lock at context + 0x718.
static int apStartLooksRight(uintptr_t seg0)
{
    static const uint8_t head[] = {
        0xf8, 0xb5, 0x04, 0x46, 0xc5, 0x6e, 0x05, 0xf5, 0xe3, 0x66, 0x30, 0x46
    };
    const volatile uint8_t *code = (const volatile uint8_t *)(seg0 + AP_START_AT);
    for (uint32_t i = 0; i < sizeof(head); i++) {
        if (code[i] != head[i])
            return 0;
    }
    return 1;
}

static int apHookInstalled(SceUID *uid)
{
    if (*uid >= 0)
        return 0;
    apFail(VITA_AUTO_AP_FAILURE_HOOK, *uid);
    *uid = -1;
    return -1;
}

static int apHookOffset(SceUID *uid, tai_hook_ref_t *ref, SceUID modid, uint32_t offset,
                        const void *hook)
{
    *uid = taiHookFunctionOffsetForKernel(KERNEL_PID, ref, modid, 0, offset, 1, hook);
    return apHookInstalled(uid);
}

static int apHookImport(SceUID *uid, tai_hook_ref_t *ref, uint32_t nid, const void *hook)
{
    *uid = taiHookFunctionImportForKernel(KERNEL_PID, ref, "SceWlanBt", AP_SDIF_LIB_NID,
                                          nid, hook);
    return apHookInstalled(uid);
}

static void apUnhook(SceUID *uid, tai_hook_ref_t ref)
{
    if (*uid >= 0) {
        taiHookReleaseForKernel(*uid, ref);
        *uid = -1;
    }
}

static int apHookData(SceUID modid, uintptr_t seg0, uintptr_t seg1)
{
    if (g_apModuleNid != AP_MODULE_NID || !apStartLooksRight(seg0)) {
        apFail(VITA_AUTO_AP_FAILURE_FIRMWARE, AP_START_AT);
        return -1;
    }
    g_apWlanCtx = (uint8_t *)(seg1 + AP_WLAN_CTX_AT);
    g_apWlanSeg0 = seg0;
    if (apHookOffset(&g_apStartHook, &g_apStartRef, modid, AP_START_AT,
                     (const void *)apStartHook) < 0 ||
        apHookImport(&g_apSdifWriteHook, &g_apSdifWriteRef, AP_SDIF_WRITE_NID,
                     (const void *)apSdifWriteHook) < 0 ||
        apHookImport(&g_apSdifWriteAsyncHook, &g_apSdifWriteAsyncRef, AP_SDIF_WRITE_ASYNC_NID,
                     (const void *)apSdifWriteAsyncHook) < 0)
        return -1;
    return 0;
}

static int apHook(void)
{
    SceUID modid;
    uintptr_t seg0;
    uintptr_t seg1;
    static const uint32_t entries[] = { AP_ALLOC_AT, AP_ENQUEUE_AT, AP_RESPONSE_AT };

    if (apFindWlan(&modid, &seg0, &seg1) < 0) {
        apFail(VITA_AUTO_AP_FAILURE_NO_MODULE, 0);
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (!apLooksLikeEntry(seg0 + entries[i])) {
            apFail(VITA_AUTO_AP_FAILURE_FIRMWARE, (int)entries[i]);
            return -1;
        }
    }
    g_apAlloc = (ApAllocFn)((seg0 + AP_ALLOC_AT) | 1);
    g_apEnqueue = (ApEnqueueFn)((seg0 + AP_ENQUEUE_AT) | 1);
    if (apHookOffset(&g_apEnqueueHook, &g_apEnqueueRef, modid, AP_ENQUEUE_AT,
                     (const void *)apEnqueueHook) < 0 ||
        apHookOffset(&g_apResponseHook, &g_apResponseRef, modid, AP_RESPONSE_AT,
                     (const void *)apResponseHook) < 0)
        return -1;
    return apHookData(modid, seg0, seg1);
}

static int apWorkerMain(SceSize args, void *argp)
{
    uint8_t body[192];
    uint64_t startUs;
    int bssStarted = 0;
    int freshAdapter;
    int result;
    (void)args; (void)argp;

    if (apHook() < 0)
        return 0;

    startUs = ksceKernelGetSystemTimeWide();
    freshAdapter = g_apAdapter == NULL;
    while (g_apAdapter == NULL) {
        if (apShouldStop())
            return 0;
        if (ksceKernelGetSystemTimeWide() - startUs > AP_ADAPTER_WAIT_US) {
            apFail(VITA_AUTO_AP_FAILURE_NO_ADAPTER, 0);
            return 0;
        }
        ksceKernelDelayThread(AP_POLL_US);
    }
    g_apAdapterWaitMs = (uint32_t)((ksceKernelGetSystemTimeWide() - startUs) / 1000);
    // A driver that just issued a command is still busy: let it settle first (a cached adapter is already quiet).
    if (freshAdapter) {
        for (uint64_t settleUs = 0; settleUs < AP_SETTLE_US; settleUs += AP_POLL_US) {
            if (apShouldStop())
                return 0;
            ksceKernelDelayThread(AP_POLL_US);
        }
    }

    for (int variant = 0; variant < 2; variant++) {
        result = apCommand(AP_CMD_SYS_CONFIG, body, apBuildConfig(body, variant));
        if (result == 0) {
            g_apConfigVariant = variant;
            break;
        }
        if (variant == 1 || apShouldStop()) {
            if (!g_apExit && g_apState != VITA_AUTO_AP_FAILED)
                apFail(VITA_AUTO_AP_FAILURE_SYS_CONFIG, result);
            return 0;
        }
    }
    if (apShouldStop())
        return 0;
    result = apCommand(AP_CMD_BSS_START, NULL, 0);
    // Even without a reply the firmware may have started; stop it either way.
    bssStarted = 1;
    if (result != 0) {
        apFail(VITA_AUTO_AP_FAILURE_BSS_START, result);
    } else {
        g_apState = VITA_AUTO_AP_UP;
        ksceDebugPrintf("[VitaAutoAP] Up, config variant %d\n", g_apConfigVariant);
        while (!apShouldStop()) {
            apDrain(0);
            ksceKernelDelayThread(AP_POLL_US);
        }
    }
    if (bssStarted) {
        g_apStopResult = apCommand(AP_CMD_BSS_STOP, NULL, 0);
        // Free the driver's command slot: a node left there blocks every later command.
        for (int i = 0; i < 20 && apPendingCommand() != 0; i++)
            ksceKernelDelayThread(AP_POLL_US / 10);
        g_apStopPending = apPendingCommand();
    }
    return 0;
}

static void apTeardown(void)
{
    if (g_apWorker >= 0) {
        g_apExit = 1;
        ksceKernelWaitThreadEnd(g_apWorker, NULL, NULL);
        ksceKernelDeleteThread(g_apWorker);
        g_apWorker = -1;
    }
    apUnhook(&g_apEnqueueHook, g_apEnqueueRef);
    apUnhook(&g_apResponseHook, g_apResponseRef);
    apUnhook(&g_apStartHook, g_apStartRef);
    apUnhook(&g_apSdifWriteHook, g_apSdifWriteRef);
    apUnhook(&g_apSdifWriteAsyncHook, g_apSdifWriteAsyncRef);
    if (g_apState != VITA_AUTO_AP_FAILED)
        g_apState = VITA_AUTO_AP_OFF;
}

static int apIsActive(void)
{
    return g_apWorker >= 0 || g_apEnqueueHook >= 0 || g_apResponseHook >= 0 ||
           g_apStartHook >= 0 || g_apSdifWriteHook >= 0 || g_apSdifWriteAsyncHook >= 0;
}

static int apStart(const VitaAutoApConfig *userConfig)
{
    int result = 0;
    VitaAutoApConfig config;
    if (userConfig == NULL ||
        ksceKernelMemcpyUserToKernel(&config, userConfig, sizeof(config)) < 0)
        return -1;
    config.ssid[sizeof(config.ssid) - 1] = '\0';
    config.passphrase[sizeof(config.passphrase) - 1] = '\0';
    {
        const size_t pass = strnlen(config.passphrase, sizeof(config.passphrase));
        if (config.ssid[0] == '\0' || config.channel < 1 || config.channel > 11 ||
            (pass != 0 && pass < 8))
            return -2;
    }
    if (!__sync_bool_compare_and_swap(&g_apLifecycleBusy, 0, 1))
        return -1;
    if (g_apWorker >= 0 && g_apState != VITA_AUTO_AP_FAILED)
        goto done; // Already starting or up
    apTeardown();
    g_apConfig = config;
    g_apFailureStage = VITA_AUTO_AP_FAILURE_NONE;
    g_apFailureResult = 0;
    g_apConfigVariant = -1;
    g_apAdapterWaitMs = 0;
    g_apCommandsSeen = 0;
    g_apCommandFailure = 0;
    g_apPoolMask = 0;
    g_apPendingAtFail = 0;
    g_apResponses = 0;
    g_apAssociations = 0;
    g_apDeauths = 0;
    g_apLastEvent = 0;
    g_apTxKicks = 0;
    g_apTxFrames = 0;
    g_apTxStamped = 0;
    memset(g_apLastStation, 0, sizeof(g_apLastStation));
    g_apRingTail = g_apRingHead;
    g_apExit = 0;
    g_apLastPingUs = ksceKernelGetSystemTimeWide();
    g_apState = VITA_AUTO_AP_STARTING;
    g_apWorker = ksceKernelCreateThread("vita_auto_ap", apWorkerMain, 0x3C, 0x4000, 0, 0x10000, 0);
    if (g_apWorker < 0) {
        result = g_apWorker;
        g_apWorker = -1;
        g_apState = VITA_AUTO_AP_OFF;
        goto done;
    }
    ksceKernelStartThread(g_apWorker, 0, NULL);
done:
    __sync_lock_release(&g_apLifecycleBusy);
    return result;
}

static int apStop(void)
{
    if (!__sync_bool_compare_and_swap(&g_apLifecycleBusy, 0, 1))
        return -1;
    apTeardown();
    g_apState = VITA_AUTO_AP_OFF;
    __sync_lock_release(&g_apLifecycleBusy);
    return 0;
}

static int apKeepAlive(void)
{
    g_apLastPingUs = ksceKernelGetSystemTimeWide();
    return g_apState;
}

static int apGetInfo(VitaAutoApInfo *userInfo)
{
    VitaAutoApInfo info;
    if (userInfo == NULL)
        return -1;
    memset(&info, 0, sizeof(info));
    info.state = g_apState;
    info.failureStage = g_apFailureStage;
    info.failureResult = g_apFailureResult;
    info.configVariant = g_apConfigVariant;
    info.adapterWaitMs = g_apAdapterWaitMs;
    info.moduleNid = g_apModuleNid;
    info.commandsSeen = g_apCommandsSeen;
    info.responses = g_apResponses;
    info.associations = g_apAssociations;
    info.deauths = g_apDeauths;
    memcpy(info.lastStation, g_apLastStation, sizeof(info.lastStation));
    info.lastEvent = g_apLastEvent;
    if (g_apWlanCtx != NULL)
        info.wlanFlags = *(volatile uint32_t *)(g_apWlanCtx + AP_CTX_FLAGS);
    info.txKicks = g_apTxKicks;
    info.txFrames = g_apTxFrames;
    info.txStamped = g_apTxStamped;
    info.commandFailure = g_apCommandFailure;
    info.poolMask = g_apPoolMask;
    info.pendingAtFail = g_apPendingAtFail;
    info.stopResult = g_apStopResult;
    info.stopPending = g_apStopPending;
    return ksceKernelMemcpyKernelToUser(userInfo, &info, sizeof(info));
}

