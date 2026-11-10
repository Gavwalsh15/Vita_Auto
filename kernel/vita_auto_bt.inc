#define BT_POLL_US           5000
#define BT_DISCOVERABLE_RETRY_US 1000000
#define BT_EVENT_LOG_SIZE    32

#define BT_NAME_CATEGORY     "/CONFIG/SYSTEM"
#define BT_NAME_KEY          "username"
#define BT_NAME_MAX          64
#define BT_RX_FIFO_SIZE      8192
#define BT_RECV_CHUNK        1024
#define BT_SEND_MAX          0x1000
#define BT_EVENT_INCOMING_CLASS 0x08
#define BT_EVENT_CONNECTED   0x05
#define BT_EVENT_DISCONNECTED 0x06
#define BT_CLASS_MAJOR_PHONE 2

#define BT_OPTION_CLIENT     0x4
static volatile int g_btcEnabled;
static SceUID g_btcThread = -1;
static int btcInstall(void);
static void btcRemove(void);
static void btcRequestOpen(uint32_t mac0, uint32_t mac1);
static void btcReset(void);
static int btcWrite(const void *userBuffer, uint32_t length);
static void btcFillInfo(VitaAutoBtInfo *info);

#define BT_FLAG_EXIT         0x01
#define BT_FLAG_WAKE         0x02
#define BT_READ_RX           0x01
#define BT_READ_WAKE         0x02

#define BT_SDP_RECORD_SIZE   0x53
static const uint8_t g_btSdpSearch[] = {
    0x53, 0x08, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x0f
};
#define BT_SDP_SEARCH_LAST_BYTE 0x22

static const uint8_t g_btSdpRecord[BT_SDP_RECORD_SIZE] = {
    0x08, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x11,
    0x19, 0x00, 0x01, 0x35, 0x14, 0x19, 0xaa, 0x01,
    0x1c, 0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11,
    0xe6, 0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a,
    0x66, 0x11, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
    0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03,
    0x08, 0x07, 0x06, 0x02, 0x00, 0x09, 0x00, 0x00,
    0x06, 0x02, 0x01, 0x09, 0x00, 0x00, 0x05, 0x02,
    0x02, 0x08, 0x00, 0x05, 0x02, 0x03, 0x08, 0x00,
    0x05, 0x02, 0x04, 0x08, 0x00, 0x05, 0x02, 0x05,
    0x08, 0x00, 0x22
};

#define BT_SEARCH_AT         0x19afc
#define BT_SEARCH_SIZE       0x20
#define BT_SEARCH_RECORD_AT  0x1faac // Where the record must have been found 
#define BT_SEARCH_BASE_TAIL  0x1f848 // Bluetooth base UUID tail, the original memcmp operand 
#define BT_SEARCH_UUID_IN_REC 17     // The 128-bit UUID inside our record 
static const uint8_t g_btSearchOriginal[BT_SEARCH_SIZE] = {
    0x4f, 0xf6, 0x48, 0x00, 0x61, 0x1d, 0xc0, 0xf2,
    0x01, 0x20, 0x0c, 0x22, 0xe9, 0xf7, 0x44, 0xec,
    0x54, 0x46, 0x00, 0x28, 0xe5, 0xd1, 0x48, 0x46,
    0x05, 0xf1, 0x01, 0x09, 0x03, 0xf0, 0x80, 0xf8,
};
static const uint8_t g_btSearchPatched[BT_SEARCH_SIZE] = {
    0x4f, 0xf6, 0xbd, 0x20, 0x61, 0x1c, 0xc0, 0xf2,
    0x01, 0x20, 0x10, 0x22, 0xe9, 0xf7, 0x44, 0xec,
    0x54, 0x46, 0x00, 0x28, 0xe5, 0xd1, 0x48, 0x46,
    0x05, 0xf1, 0x01, 0x09, 0x4a, 0xf6, 0x01, 0x20,
};
static SceUID g_btSearchUid = -1;
static volatile uint8_t g_btSearchSeen[64];
static uint8_t g_btSearchWant[BT_SEARCH_SIZE];
static uint8_t g_btSearchNew[BT_SEARCH_SIZE];

// Thumb-2 MOVW/MOVT into r0 (top = 1 for MOVT), little-endian halfwords
static void btEncodeMov16(uint8_t *out, uint32_t imm16, int top)
{
    const uint16_t h1 = (uint16_t)((top ? 0xF2C0 : 0xF240) | (((imm16 >> 11) & 1) << 10)
                                   | ((imm16 >> 12) & 0xf));
    const uint16_t h2 = (uint16_t)((((imm16 >> 8) & 7) << 12) | (imm16 & 0xff));
    out[0] = (uint8_t)h1; out[1] = (uint8_t)(h1 >> 8);
    out[2] = (uint8_t)h2; out[3] = (uint8_t)(h2 >> 8);
}

// Puts the address of a SceBt byte into the movw (bytes 0-3) and movt (6-9)
static void btSetAddress(uint8_t *code, uint32_t address)
{
    btEncodeMov16(code + 0, address & 0xffff, 0);
    btEncodeMov16(code + 6, address >> 16, 1);
}
static volatile int g_btSearchResult;
static volatile int g_btSearchVerified;

static volatile int g_btState = VITA_AUTO_BT_OFF;
static volatile int g_btFailureStage;
static volatile int g_btFailureResult;
static volatile uint32_t g_btEventCount;
static uint32_t g_btOptions;

#define BT_OPTION_RFCOMM 0x2
#define BT_RFCOMM_AT   0x15b4a // PN-response store block
#define BT_RFCOMM_SIZE 0x14    // five strb.w instructions, through the K store
// strb.w {lr,r6,r4,r4,r4} -> [sp+16,17,21,24,25]; the last one stores K.
static const uint8_t g_btRfcommOriginal[BT_RFCOMM_SIZE] = {
    0x8d,0xf8,0x10,0xe0, 0x8d,0xf8,0x11,0x60, 0x8d,0xf8,0x15,0x40,
    0x8d,0xf8,0x18,0x40, 0x8d,0xf8,0x19,0x40
};
// Only the last byte differs: the K store's Rt, r4 (0x40) -> r2 (0x20)
static const uint8_t g_btRfcommPatched[BT_RFCOMM_SIZE] = {
    0x8d,0xf8,0x10,0xe0, 0x8d,0xf8,0x11,0x60, 0x8d,0xf8,0x15,0x40,
    0x8d,0xf8,0x18,0x40, 0x8d,0xf8,0x19,0x20
};
static SceUID g_btRfcommUid = -1;
static volatile int g_btRfcommResult;
static volatile int g_btRfcommVerified;

#define BT_RFCOMM_SENDER_AT  0x153ec // frame sender 0x020153ec 
#define BT_RFCOMM_DISP_AT    0x401c  // frame dispatcher 0x0200401c 
#define BT_RFCOMM_FLOW_READY 0x2000  // [DLC+0x260] bit the sender needs to send data
#define BT_RFCOMM_DESC_DATA  0x40000 // incoming credit-based data: no dispatcher branch
#define BT_RFCOMM_DESC_SPP   0x80000 // dispatcher branch to the SPP receive ring
static int vitaAutoHookContinue(tai_hook_ref_t ref, int a0, int a1, int a2, int a3, int a4, int a5)
{
    struct _tai_hook_user *cur = (struct _tai_hook_user *)ref;
    struct _tai_hook_user *next = (struct _tai_hook_user *)cur->next;
    int (*cont)(int, int, int, int, int, int) =
        (int (*)(int, int, int, int, int, int))(next == NULL ? cur->old : next->func);
    return cont(a0, a1, a2, a3, a4, a5);
}

static tai_hook_ref_t g_btSenderHookRef;
static SceUID g_btSenderHookUid = -1;
static tai_hook_ref_t g_btRxHookRef;
static SceUID g_btRxHookUid = -1;
static volatile int g_btHookResult; // 0, or the first hook install error
// The last data-DLCI send, as the sender found it (before the flow-ready fix).
static volatile uint32_t g_btSenderCalls;
static volatile uint32_t g_btDlcAddrByte; // RFCOMM address octet (DLCI in bits 2-7)
static volatile uint32_t g_btDlcPending;  // [DLC+0x48]  bytes queued to send
static volatile uint32_t g_btDlcCredits;  // [DLC+0x254] send credits
static volatile uint32_t g_btDlcFlags;    // [DLC+0x260] flow flags
// Incoming frames by dispatcher branch, and the last one no branch handled.
static volatile uint32_t g_btRxDispCalls;
static volatile uint32_t g_btRxToEnq;     // to the SPP receive ring (bit19)
static volatile uint32_t g_btRxToCredit;  // to the credit handler
static volatile uint32_t g_btRxRerouted;  // dropped data frames sent to the ring instead
static volatile uint32_t g_btRxLastDesc;
static volatile uint32_t g_btRxLastLen;
static volatile uint32_t g_btRxLastB0;    // its first 8 bytes
static volatile uint32_t g_btRxLastB1;
static volatile int g_btPatchVerified; // 1 once the record reads back from SceBt
static volatile uint32_t g_btPatchOffset;
static char g_btWantedName[BT_NAME_MAX]; // Empty: leave the name alone
static char g_btRecoverName[BT_NAME_MAX]; // Original left by a run that crashed
static char g_btOrigName[BT_NAME_MAX];
static int g_btNameChanged;
static volatile int g_btNameResult;
static int g_btDiscoverable;
static volatile int g_btDiscoverableResult;
static VitaAutoBtEvent g_btEventLog[BT_EVENT_LOG_SIZE];
static volatile uint32_t g_btEventHead;
static volatile uint32_t g_btEventTail;
static volatile uint32_t g_btEventsDropped;
static volatile int g_btPeerValid;
static volatile uint32_t g_btPeerMac0;
static volatile uint32_t g_btPeerMac1;
static volatile int g_btClaimed;
static volatile int g_btWriteBusy;
static volatile int g_btLifecycleBusy;
static SceUID g_btWorker = -1;
static SceUID g_btWorkerFlags = -1;
static SceUID g_btReadEvent = -1;
static SceUID g_btCallback = -1;
static SceUID g_btPatchUid = -1;

// Single producer (worker), single consumer (syscall reader).
static uint8_t g_btRxFifo[BT_RX_FIFO_SIZE];
static volatile uint32_t g_btRxHead;
static volatile uint32_t g_btRxTail;
static volatile uint32_t g_btTxCalls;
static volatile uint32_t g_btTxBytes;
static volatile int g_btLastSendResult;
static volatile uint32_t g_btRxCalls;
static volatile uint32_t g_btRxBytes;
static volatile int g_btLastRecvResult;
static uint8_t g_btRecvChunk[BT_RECV_CHUNK] __attribute__((aligned(64)));
static uint8_t g_btSendBuffer[BT_SEND_MAX] __attribute__((aligned(64)));

static void btFail(int stage, int result)
{
    g_btFailureStage = stage;
    g_btFailureResult = result;
    g_btState = VITA_AUTO_BT_FAILED;
    ksceDebugPrintf("[VitaAutoBT] failure stage %d result 0x%08x\n", stage, result);
}

static void btWakeReader(unsigned int bits)
{
    if (g_btReadEvent >= 0)
        ksceKernelSetEventFlag(g_btReadEvent, bits);
}

static uint32_t btFifoUsed(void)
{
    return (g_btRxHead - g_btRxTail) % BT_RX_FIFO_SIZE;
}

static void btFifoReset(void)
{
    g_btRxTail = g_btRxHead;
}

// One slot stays empty so full and empty differ. Overflow is dropped; the app treats a gap as a broken handshake.
static void btFifoPush(const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        const uint32_t head = g_btRxHead;
        const uint32_t next = (head + 1) % BT_RX_FIFO_SIZE;
        if (next == g_btRxTail)
            break;
        g_btRxFifo[head] = data[i];
        __sync_synchronize();
        g_btRxHead = next;
    }
}

static void btNameApply(void)
{
    char current[BT_NAME_MAX];
    int result;

    if (g_btWantedName[0] == '\0')
        return; // Renaming is switched off in the app's settings.

    memset(current, 0, sizeof(current));
    result = ksceRegMgrGetKeyStr(BT_NAME_CATEGORY, BT_NAME_KEY, current, sizeof(current));
    if (result < 0) {
        g_btNameResult = result;
        return;
    }
    if (strncmp(current, g_btWantedName, sizeof(current)) == 0) {
        if (g_btRecoverName[0] != '\0') {
            memcpy(g_btOrigName, g_btRecoverName, sizeof(g_btOrigName));
            g_btNameChanged = 1;
        }
        g_btNameResult = 0;
        return;
    }
    memcpy(g_btOrigName, current, sizeof(g_btOrigName));
    result = ksceRegMgrSetKeyStr(
        BT_NAME_CATEGORY, BT_NAME_KEY, g_btWantedName, strlen(g_btWantedName) + 1);
    g_btNameResult = result;
    if (result >= 0)
        g_btNameChanged = 1;
}

static void btNameRestore(void)
{
    if (!g_btNameChanged)
        return;
    ksceRegMgrSetKeyStr(BT_NAME_CATEGORY, BT_NAME_KEY, g_btOrigName,
                        strlen(g_btOrigName) + 1);
    g_btNameChanged = 0;
}

static int btFindSceBt(SceKernelModuleInfo *info, SceUID *modid)
{
    SceUID mods[128];
    SceSize num = 128;
    if (ksceKernelGetModuleList(0x10005, 0x7FFFFFFF, 1, mods, &num) < 0)
        return -1;
    for (SceSize i = 0; i < num; i++) {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        if (ksceKernelGetModuleInfo(0x10005, mods[i], info) < 0)
            continue;
        if (strncmp(info->module_name, "SceBt", 6) == 0) {
            *modid = mods[i];
            return 0;
        }
    }
    return -1;
}

static const uint8_t *btSegment0(SceUID *modid, uint32_t *size)
{
    SceKernelModuleInfo info;
    if (btFindSceBt(&info, modid) < 0)
        return NULL;
    *size = info.segments[0].memsz;
    return (const uint8_t *)info.segments[0].vaddr;
}

static int btInject(SceUID modid, uint32_t offset, const void *data, uint32_t size, SceUID *uid)
{
    *uid = taiInjectDataForKernel(KERNEL_PID, modid, 0, offset, data, size);
    if (*uid < 0) {
        const int result = *uid;
        *uid = -1;
        return result;
    }
    return 0;
}

static int btPatchSdp(void)
{
    SceUID modid;
    uint32_t size;
    uint32_t found = 0;
    uint32_t offset = 0;
    const uint8_t *base = btSegment0(&modid, &size);

    if (base == NULL || size < sizeof(g_btSdpSearch) + BT_SDP_RECORD_SIZE)
        return VITA_AUTO_BT_FAILURE_NO_MODULE;

    for (uint32_t p = 0; p + BT_SDP_RECORD_SIZE + 1 <= size; p++) {
        if (memcmp(base + p, g_btSdpSearch, sizeof(g_btSdpSearch)) != 0)
            continue;
        if (base[p + BT_SDP_RECORD_SIZE] != BT_SDP_SEARCH_LAST_BYTE)
            continue;
        found++;
        offset = p + 1;
    }
    if (found != 1) {
        g_btFailureResult = (int)found;
        return VITA_AUTO_BT_FAILURE_RECORD_NOT_FOUND;
    }

    {
        const int result = btInject(modid, offset, g_btSdpRecord, BT_SDP_RECORD_SIZE, &g_btPatchUid);
        if (result < 0) {
            g_btFailureResult = result;
            return VITA_AUTO_BT_FAILURE_PATCH;
        }
    }

    g_btPatchOffset = offset;
    g_btPatchVerified = memcmp(base + offset, g_btSdpRecord, BT_SDP_RECORD_SIZE) == 0;
    return 0;
}

static int btPatchSearch(void)
{
    SceUID modid;
    uint32_t size;
    const uint8_t *base = btSegment0(&modid, &size);
    int result;

    if (base == NULL || size < BT_SEARCH_AT + BT_SEARCH_SIZE)
        return -1;
    for (uint32_t i = 0; i < sizeof(g_btSearchSeen); i++)
        g_btSearchSeen[i] = base[BT_SEARCH_AT - 16 + i];
    if (g_btPatchOffset != BT_SEARCH_RECORD_AT)
        return -3;
    memcpy(g_btSearchWant, g_btSearchOriginal, BT_SEARCH_SIZE);
    btSetAddress(g_btSearchWant, (uint32_t)base + BT_SEARCH_BASE_TAIL);
    if (memcmp(base + BT_SEARCH_AT, g_btSearchWant, BT_SEARCH_SIZE) != 0)
        return -2;
    memcpy(g_btSearchNew, g_btSearchPatched, BT_SEARCH_SIZE);
    btSetAddress(g_btSearchNew,
        (uint32_t)base + BT_SEARCH_RECORD_AT + BT_SEARCH_UUID_IN_REC);
    result = btInject(modid, BT_SEARCH_AT, g_btSearchNew, BT_SEARCH_SIZE, &g_btSearchUid);
    if (result < 0)
        return result;
    g_btSearchVerified =
        memcmp(base + BT_SEARCH_AT, g_btSearchNew, BT_SEARCH_SIZE) == 0;
    return 0;
}

static int btPatchRfcomm(void)
{
    SceUID modid;
    uint32_t size;
    const uint8_t *base = btSegment0(&modid, &size);
    int result;

    if (base == NULL || size < BT_RFCOMM_AT + BT_RFCOMM_SIZE)
        return -1;
    if (memcmp(base + BT_RFCOMM_AT, g_btRfcommOriginal, BT_RFCOMM_SIZE) != 0)
        return -2;
    result = btInject(modid, BT_RFCOMM_AT, g_btRfcommPatched, BT_RFCOMM_SIZE, &g_btRfcommUid);
    if (result < 0)
        return result;
    g_btRfcommVerified =
        memcmp(base + BT_RFCOMM_AT, g_btRfcommPatched, BT_RFCOMM_SIZE) == 0;
    return 0;
}

static int btSenderHook(int a0, int a1, int a2, int a3, int a4, int a5)
{
    const uintptr_t dlc = (uintptr_t)(unsigned int)a1;
    if (dlc != 0 && (((unsigned int)a2 >> 2) & 0x3f) != 0) {
        g_btDlcAddrByte = (unsigned int)a2 & 0xff;
        g_btDlcPending = *(volatile uint32_t *)(dlc + 0x48);
        g_btDlcCredits = *(volatile uint8_t *)(dlc + 0x254);
        g_btDlcFlags = *(volatile uint32_t *)(dlc + 0x260);
        ++g_btSenderCalls;
        *(volatile uint32_t *)(dlc + 0x260) = g_btDlcFlags | BT_RFCOMM_FLOW_READY;
    }
    return vitaAutoHookContinue(g_btSenderHookRef, a0, a1, a2, a3, a4, a5);
}

static int btRxDispHook(int a0, int a1, int a2, int a3)
{
    unsigned int desc = (unsigned int)a1;
    const unsigned int len = (unsigned int)a3;
    int routed = 1;
    ++g_btRxDispCalls;
    if (desc & 0x10000) {
        // -> 0x020118e4
    } else if (desc & 0x2000000c) {
        ++g_btRxToCredit; // -> 0x02007984
    } else if (desc & 0x2000) {
        // -> 0x0200b630
    } else if (desc & 0x1000) {
        // -> 0x0200bfd8
    } else if (desc & 0x300) {
        // -> 0x020137b0
    } else if (desc & BT_RFCOMM_DESC_SPP) {
        ++g_btRxToEnq; // -> 0x0201ad58, the SPP receive ring
    } else {
        routed = 0; // the dispatcher would drop it
    }
    if (!routed && len != 0 && len <= 0x400 && a2 != 0) {
        const uintptr_t p = (uintptr_t)(unsigned int)a2;
        g_btRxLastDesc = desc;
        g_btRxLastLen = len;
        g_btRxLastB0 = len >= 4 ? *(volatile uint32_t *)p : 0;
        g_btRxLastB1 = len >= 8 ? *(volatile uint32_t *)(p + 4) : 0;
        if (desc & BT_RFCOMM_DESC_DATA) {
            a1 = (int)(desc | BT_RFCOMM_DESC_SPP);
            ++g_btRxRerouted;
        }
    }
    return vitaAutoHookContinue(g_btRxHookRef, a0, a1, a2, a3, 0, 0);
}

static int btHookRfcomm(void)
{
    SceKernelModuleInfo info;
    SceUID modid;

    if (btFindSceBt(&info, &modid) < 0)
        return -1;
    g_btSenderHookUid = taiHookFunctionOffsetForKernel(KERNEL_PID, &g_btSenderHookRef,
        modid, 0, BT_RFCOMM_SENDER_AT, 1, (const void *)btSenderHook);
    if (g_btSenderHookUid < 0) {
        const int result = g_btSenderHookUid;
        g_btSenderHookUid = -1;
        return result;
    }
    g_btRxHookUid = taiHookFunctionOffsetForKernel(KERNEL_PID, &g_btRxHookRef,
        modid, 0, BT_RFCOMM_DISP_AT, 1, (const void *)btRxDispHook);
    if (g_btRxHookUid < 0) {
        const int result = g_btRxHookUid;
        g_btRxHookUid = -1;
        taiHookReleaseForKernel(g_btSenderHookUid, g_btSenderHookRef);
        g_btSenderHookUid = -1;
        return result;
    }
    return 0;
}

// Remember the "incoming class" event (0x08) so a PC or controller is not taken for the phone.
static volatile uint32_t g_btClassMac0;
static volatile uint32_t g_btClassMac1;
static volatile uint32_t g_btClassValue;
static volatile int g_btClassKnown;

static int btConnectIsPhone(uint32_t mac0, uint32_t mac1)
{
    if (g_btClassKnown && g_btClassMac0 == mac0 && g_btClassMac1 == mac1)
        return ((g_btClassValue >> 8) & 0x1f) == BT_CLASS_MAJOR_PHONE;
    return 1; // No class seen
}

// Runs in the worker thread (inside the CB wait): only read the event and record it.
static int btCallback(int notifyId, int notifyCount, int notifyArg, void *common)
{
    SceBtEvent ev;
    int ret;
    (void)notifyId; (void)notifyCount; (void)notifyArg; (void)common;

    while ((ret = ksceBtReadEvent(&ev, 1)) == (int)SCE_BT_ERROR_CB_OVERFLOW)
        ;
    if (ret <= 0)
        return 0;

    ++g_btEventCount;
    {
        // Raw copy for the app log; full means the app is not draining.
        const uint32_t head = g_btEventHead;
        const uint32_t next = (head + 1) % BT_EVENT_LOG_SIZE;
        if (next == g_btEventTail) {
            ++g_btEventsDropped;
        } else {
            g_btEventLog[head].timeMs = ksceKernelGetSystemTimeLow() / 1000;
            memcpy(g_btEventLog[head].data, ev.data, sizeof(ev.data));
            __sync_synchronize();
            g_btEventHead = next;
        }
    }
    if (ev.id == BT_EVENT_INCOMING_CLASS) {
        g_btClassMac0 = ev.mac0;
        g_btClassMac1 = ev.mac1;
        g_btClassValue = ev.unk3 & 0xffffffu;
        g_btClassKnown = 1;
    } else if (ev.id == BT_EVENT_CONNECTED) {
        if (btConnectIsPhone(ev.mac0, ev.mac1)) {
            g_btPeerMac0 = ev.mac0;
            g_btPeerMac1 = ev.mac1;
            __sync_synchronize();
            g_btPeerValid = 1;
            if (g_btState == VITA_AUTO_BT_READY)
                g_btState = VITA_AUTO_BT_LINKED;
            if (g_btcEnabled)
                btcRequestOpen(ev.mac0, ev.mac1);
            btWakeReader(BT_READ_WAKE);
        }
    } else if (ev.id == BT_EVENT_DISCONNECTED) {
        // Only the phone's own disconnect ends the link; a PC leaving must not.
        if (g_btPeerValid && ev.mac0 == g_btPeerMac0 && ev.mac1 == g_btPeerMac1) {
            g_btPeerValid = 0;
            if (g_btState == VITA_AUTO_BT_LINKED)
                g_btState = VITA_AUTO_BT_READY;
            if (g_btcEnabled)
                btcReset();
            btWakeReader(BT_READ_WAKE);
        }
    }
    return 0;
}

// Discoverable: SceBt refuses (0x802f1503) until the stack is up, so retry until it sticks.
static void btTryDiscoverable(void)
{
    if (g_btDiscoverable)
        return;
    g_btDiscoverableResult = ksceBtSetInquiryScan(1);
    if (g_btDiscoverableResult >= 0) {
        g_btDiscoverable = 1;
        vitaUsbLogInfo("[VitaAutoBT] Discoverable\n");
    }
}

static void btPollReceive(void)
{
    int ret;
    if (!g_btClaimed || !g_btPeerValid || g_btcEnabled)
        return; // In client mode the iAP2 thread fills the FIFO
    ret = ksceBtRecvSpp(
        (int)g_btPeerMac0, (int)g_btPeerMac1, (int)g_btRecvChunk, BT_RECV_CHUNK);
    g_btRxCalls++;
    g_btLastRecvResult = ret;
    if (ret > BT_RECV_CHUNK)
        ret = BT_RECV_CHUNK;
    if (ret > 0) {
        g_btRxBytes += (uint32_t)ret;
        btFifoPush(g_btRecvChunk, (uint32_t)ret);
        btWakeReader(BT_READ_RX);
    }
}

static int btWorkerMain(SceSize args, void *argp)
{
    int result;
    (void)args; (void)argp;

    if (ksceBtGetConfiguration() <= 0) {
        btFail(VITA_AUTO_BT_FAILURE_BT_DISABLED, 0);
        return 0;
    }

    btNameApply();
    result = btPatchSdp();
    if (result != 0) {
        btFail(result, g_btFailureResult);
        return 0;
    }
    // Without this the phone's search finds no channel, but pairing and the link still work: not fatal.
    g_btSearchResult = btPatchSearch();
    // Optional: the hooks go in only once the patch confirms this is the firmware their offsets came from.
    if (g_btOptions & BT_OPTION_RFCOMM) {
        g_btRfcommResult = btPatchRfcomm();
        if (g_btRfcommResult == 0)
            g_btHookResult = btHookRfcomm();
    }
    // Needs the search patch's check of the firmware, so it comes after it.
    if (g_btOptions & BT_OPTION_CLIENT)
        btcInstall();

    g_btCallback = ksceKernelCreateCallback("vita_auto_bt_cb", 0, btCallback, NULL);
    if (g_btCallback < 0) {
        btFail(VITA_AUTO_BT_FAILURE_CALLBACK, g_btCallback);
        return 0;
    }
    result = ksceBtRegisterCallback(g_btCallback, 0, 0xFFFFFFFF, 0xFFFFFFFF);
    if (result < 0) {
        btFail(VITA_AUTO_BT_FAILURE_CALLBACK, result);
        return 0;
    }
    g_btState = VITA_AUTO_BT_READY;
    vitaUsbLogInfo("[VitaAutoBT] Ready\n");
    btTryDiscoverable();

    for (;;) {
        unsigned int out = 0;
        SceUInt timeout = BT_POLL_US;
        // Poll only while a session owns the link, else sleep until an event, claim or exit (also wake to retry discoverable).
        const int polling = g_btClaimed && g_btPeerValid && !g_btcEnabled;
        if (!polling && !g_btDiscoverable)
            timeout = BT_DISCOVERABLE_RETRY_US;
        ksceKernelWaitEventFlagCB(g_btWorkerFlags, BT_FLAG_EXIT | BT_FLAG_WAKE,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &out,
            (polling || !g_btDiscoverable) ? &timeout : NULL);
        if (out & BT_FLAG_EXIT)
            break;
        btTryDiscoverable();
        if (polling)
            ksceKernelDelayThread(BT_POLL_US);
        btPollReceive();
    }
    // Unregister must run on the thread that registered (SceBt keys callbacks by thread); from Disable it leaked
    // a slot per run until registering failed (0x802f0c02).
    ksceBtUnregisterCallback(g_btCallback);
    return 0;
}

static int btInit(void)
{
    g_btWorkerFlags = ksceKernelCreateEventFlag(
        "vita_auto_bt_w", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    g_btReadEvent = ksceKernelCreateEventFlag(
        "vita_auto_bt_rx", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    return (g_btWorkerFlags < 0 || g_btReadEvent < 0) ? -1 : 0;
}

static int btIsActive(void)
{
    return g_btWorker >= 0 || g_btcThread >= 0 || g_btCallback >= 0 || g_btPatchUid >= 0 || g_btSearchUid >= 0 || g_btRfcommUid >= 0 || g_btSenderHookUid >= 0 || g_btRxHookUid >= 0;
}

static void btTeardown(void)
{
    if (g_btClaimed) {
        g_btClaimed = 0;
        releaseTransport(VITA_AUTO_TRANSPORT_BT);
    }
    if (g_btWorker >= 0) {
        ksceKernelSetEventFlag(g_btWorkerFlags, BT_FLAG_EXIT);
        ksceKernelWaitThreadEnd(g_btWorker, NULL, NULL);
        ksceKernelDeleteThread(g_btWorker);
        g_btWorker = -1;
    }
    btcRemove();
    if (g_btDiscoverable) {
        ksceBtSetInquiryScan(0);
        g_btDiscoverable = 0;
    }
    g_btDiscoverableResult = 0;
    g_btPatchVerified = 0;
    btNameRestore();
    if (g_btCallback >= 0) {
        // The worker has already unregistered it on its way out (see btWorkerMain).
        ksceKernelDeleteCallback(g_btCallback);
        g_btCallback = -1;
    }
    if (g_btRfcommUid >= 0) {
        taiInjectReleaseForKernel(g_btRfcommUid);
        g_btRfcommUid = -1;
    }
    g_btRfcommVerified = 0;
    g_btRfcommResult = 0;
    if (g_btSenderHookUid >= 0) {
        taiHookReleaseForKernel(g_btSenderHookUid, g_btSenderHookRef);
        g_btSenderHookUid = -1;
    }
    if (g_btRxHookUid >= 0) {
        taiHookReleaseForKernel(g_btRxHookUid, g_btRxHookRef);
        g_btRxHookUid = -1;
    }
    g_btHookResult = 0;
    g_btSenderCalls = 0;
    g_btRxDispCalls = 0;
    g_btRxToEnq = 0;
    g_btRxToCredit = 0;
    g_btRxRerouted = 0;
    if (g_btSearchUid >= 0) {
        taiInjectReleaseForKernel(g_btSearchUid);
        g_btSearchUid = -1;
    }
    g_btSearchVerified = 0;
    g_btSearchResult = 0;
    if (g_btPatchUid >= 0) {
        taiInjectReleaseForKernel(g_btPatchUid);
        g_btPatchUid = -1;
    }
    g_btPeerValid = 0;
    btWakeReader(BT_READ_WAKE);
    g_btState = VITA_AUTO_BT_OFF;
}

// Syscall bodies: Enable/Disable bracket the app's lifetime, Start/Stop claim the transport per session.
static int btEnable(void)
{
    int result = 0;
    if (g_btWorkerFlags < 0 || g_btReadEvent < 0)
        return -1;
    if (!__sync_bool_compare_and_swap(&g_btLifecycleBusy, 0, 1))
        return -1;
    if (g_btState == VITA_AUTO_BT_WAITING || g_btState == VITA_AUTO_BT_READY ||
        g_btState == VITA_AUTO_BT_LINKED)
        goto done;
    btTeardown(); // A retry after a failure starts from a clean slate.
    g_btFailureStage = VITA_AUTO_BT_FAILURE_NONE;
    g_btFailureResult = 0;
    g_btEventCount = 0;
    g_btEventTail = g_btEventHead; // No worker yet, so nothing is producing.
    g_btEventsDropped = 0;
    ksceKernelClearEventFlag(g_btWorkerFlags, 0);
    g_btState = VITA_AUTO_BT_WAITING;
    g_btWorker = ksceKernelCreateThread(
        "vita_auto_bt", btWorkerMain, 0x3C, 0x4000, 0, 0x10000, 0);
    if (g_btWorker < 0) {
        result = g_btWorker;
        g_btWorker = -1;
        g_btState = VITA_AUTO_BT_OFF;
        goto done;
    }
    ksceKernelStartThread(g_btWorker, 0, NULL);
done:
    __sync_lock_release(&g_btLifecycleBusy);
    return result;
}

static int btDisable(void)
{
    if (!__sync_bool_compare_and_swap(&g_btLifecycleBusy, 0, 1))
        return -1;
    btTeardown();
    __sync_lock_release(&g_btLifecycleBusy);
    return 0;
}

static int btStart(void)
{
    if (g_btState != VITA_AUTO_BT_READY && g_btState != VITA_AUTO_BT_LINKED)
        return -1;
    if (!claimTransport(VITA_AUTO_TRANSPORT_BT))
        return VITA_AUTO_ERROR_TRANSPORT_BUSY;
    btFifoReset();
    g_btTxCalls = 0;
    g_btTxBytes = 0;
    g_btLastSendResult = 0;
    g_btRxCalls = 0;
    g_btRxBytes = 0;
    g_btLastRecvResult = 0;
    g_btClaimed = 1;
    ksceKernelSetEventFlag(g_btWorkerFlags, BT_FLAG_WAKE);
    return 0;
}

static int btStop(void)
{
    g_btClaimed = 0;
    btWakeReader(BT_READ_WAKE);
    releaseTransport(VITA_AUTO_TRANSPORT_BT);
    return 0;
}

static int btReadWaitTimed(void *userBuffer, uint32_t capacity, uint32_t timeoutUs)
{
    uint32_t total = 0;

    if (userBuffer == NULL || capacity == 0 || !g_btClaimed)
        return -1;
    if (btFifoUsed() == 0 && timeoutUs != 0 && g_btReadEvent >= 0) {
        SceUInt timeout = timeoutUs;
        unsigned int matched = 0;
        ksceKernelWaitEventFlag(g_btReadEvent, BT_READ_RX | BT_READ_WAKE,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &matched, &timeout);
    }
    if (!g_btClaimed)
        return -1;

    while (total < capacity && g_btRxTail != g_btRxHead) {
        const uint32_t tail = g_btRxTail;
        uint32_t run = (g_btRxHead > tail ? g_btRxHead : BT_RX_FIFO_SIZE) - tail;
        if (run > capacity - total)
            run = capacity - total;
        if (ksceKernelMemcpyKernelToUser(
                (unsigned char *)userBuffer + total, &g_btRxFifo[tail], run) < 0)
            return -1;
        total += run;
        __sync_synchronize();
        g_btRxTail = (tail + run) % BT_RX_FIFO_SIZE;
    }
    return (int)total;
}

static int btWrite(const void *userBuffer, uint32_t length)
{
    int result;
    if (userBuffer == NULL || length == 0 || length > BT_SEND_MAX ||
        !g_btClaimed || !g_btPeerValid)
        return -1;
    if (g_btcEnabled)
        return btcWrite(userBuffer, length);
    if (!__sync_bool_compare_and_swap(&g_btWriteBusy, 0, 1))
        return 0;
    if (ksceKernelMemcpyUserToKernel(g_btSendBuffer, userBuffer, length) < 0) {
        __sync_lock_release(&g_btWriteBusy);
        return -1;
    }
    result = ksceBtSendSpp(
        (int)g_btPeerMac0, (int)g_btPeerMac1, (int)g_btSendBuffer, (int)length);
    __sync_lock_release(&g_btWriteBusy);
    g_btTxCalls++;
    g_btLastSendResult = result;
    if (result >= 0)
        g_btTxBytes += length;
    return result < 0 ? result : (int)length;
}

static int btGetInfo(VitaAutoBtInfo *userInfo)
{
    VitaAutoBtInfo info;
    if (userInfo == NULL)
        return -1;
    info.layoutSize = sizeof(info);
    info.state = g_btState;
    info.claimed = g_btClaimed;
    info.peerValid = g_btPeerValid;
    info.failureStage = g_btFailureStage;
    info.failureResult = g_btFailureResult;
    info.peerMac0 = g_btPeerMac0;
    info.peerMac1 = g_btPeerMac1;
    info.eventCount = g_btEventCount;
    info.txCalls = g_btTxCalls;
    info.txBytes = g_btTxBytes;
    info.lastSendResult = g_btLastSendResult;
    info.rxCalls = g_btRxCalls;
    info.rxBytes = g_btRxBytes;
    info.lastRecvResult = g_btLastRecvResult;
    info.nameResult = g_btNameResult;
    info.nameChanged = g_btNameChanged;
    for (uint32_t i = 0; i < sizeof(info.searchSeen); i++)
        info.searchSeen[i] = g_btSearchSeen[i];
    info.searchPatched = g_btSearchVerified;
    info.searchResult = g_btSearchResult;
    info.rfcommPatched = g_btRfcommVerified;
    info.rfcommResult = g_btRfcommResult;
    info.rfcommHooked = g_btSenderHookUid >= 0 && g_btRxHookUid >= 0;
    info.rfcommHookResult = g_btHookResult;
    info.rfcommSenderCalls = g_btSenderCalls;
    info.dlcAddrByte = g_btDlcAddrByte;
    info.dlcPending = g_btDlcPending;
    info.dlcCredits = g_btDlcCredits;
    info.dlcFlags = g_btDlcFlags;
    info.rxDispCalls = g_btRxDispCalls;
    info.rxToEnq = g_btRxToEnq;
    info.rxToCredit = g_btRxToCredit;
    info.rxLastDesc = g_btRxLastDesc;
    info.rxLastLen = g_btRxLastLen;
    info.rxLastB0 = g_btRxLastB0;
    info.rxLastB1 = g_btRxLastB1;
    info.rxRerouted = g_btRxRerouted;
    info.patchVerified = g_btPatchVerified;
    info.patchOffset = g_btPatchOffset;
    info.discoverableResult = g_btDiscoverableResult;
    info.discoverable = g_btDiscoverable;
    info.eventsDropped = g_btEventsDropped;
    memset(&info.iap2Installed, 0, sizeof(info) - __builtin_offsetof(VitaAutoBtInfo, iap2Installed));
    btcFillInfo(&info);
    return ksceKernelMemcpyKernelToUser(userInfo, &info, sizeof(info));
}

static int btReadEvents(VitaAutoBtEvent *userEvents, uint32_t maxEvents)
{
    uint32_t count = 0;
    if (userEvents == NULL)
        return -1;
    while (count < maxEvents && g_btEventTail != g_btEventHead) {
        const uint32_t tail = g_btEventTail;
        if (ksceKernelMemcpyKernelToUser(
                &userEvents[count], &g_btEventLog[tail], sizeof(VitaAutoBtEvent)) < 0)
            return -1;
        __sync_synchronize();
        g_btEventTail = (tail + 1) % BT_EVENT_LOG_SIZE;
        ++count;
    }
    return (int)count;
}

static int btConfigureName(const char *userWanted, const char *userRecover)
{
    char wanted[BT_NAME_MAX];
    char recover[BT_NAME_MAX];
    if (g_btState != VITA_AUTO_BT_OFF && g_btState != VITA_AUTO_BT_FAILED)
        return -1;
    memset(wanted, 0, sizeof(wanted));
    memset(recover, 0, sizeof(recover));
    if (userWanted != NULL &&
        ksceKernelStrncpyUserToKernel(wanted, userWanted, sizeof(wanted) - 1) < 0)
        return -1;
    if (userRecover != NULL &&
        ksceKernelStrncpyUserToKernel(recover, userRecover, sizeof(recover) - 1) < 0)
        return -1;
    memcpy(g_btWantedName, wanted, sizeof(g_btWantedName));
    memcpy(g_btRecoverName, recover, sizeof(g_btRecoverName));
    return 0;
}

// Copies the original device name to the app while renamed: its length, or 0 if unchanged.
static int btGetOriginalName(char *userBuffer, uint32_t size)
{
    uint32_t length;
    if (userBuffer == NULL || size == 0 || !g_btNameChanged)
        return 0;
    length = strlen(g_btOrigName);
    if (length + 1 > size)
        return -1;
    if (ksceKernelMemcpyKernelToUser(userBuffer, g_btOrigName, length + 1) < 0)
        return -1;
    return (int)length;
}

// Drops the ACL link via SceBt's own disconnect; hand-sent L2CAP/RFCOMM disconnects took the system down on exit.
static int btDisconnect(uint32_t mac0, uint32_t mac1)
{
    if (g_btState != VITA_AUTO_BT_READY && g_btState != VITA_AUTO_BT_LINKED)
        return -1;
    return ksceBtStartDisconnect(mac0, mac1);
}

static int btConnectState(uint32_t mac0, uint32_t mac1)
{
    return ksceBtGetConnectingInfo(mac0, mac1);
}

static int btSetOptions(uint32_t flags)
{
    if (g_btState != VITA_AUTO_BT_OFF && g_btState != VITA_AUTO_BT_FAILED)
        return -1;
    g_btOptions = flags;
    return 0;
}
