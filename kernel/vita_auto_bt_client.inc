#define BTC_SIG_AT        0x6558  // incoming L2CAP PDU handler
#define BTC_LOCK_AT       0x1b6b4 // BT state lock, 0 on success
#define BTC_UNLOCK_AT     0x1b7a0
#define BTC_CONN_GET_AT   0x11420 // connection object by device address
#define BTC_CONN_REQ_AT   0x6422  // L2CAP CONNECTION_REQUEST sender
#define BTC_CHAN_NEW_AT   0x11544 // find or create a channel entry
#define BTC_SEND_AT       0x109ec // the one L2CAP frame sender
#define BTC_STATE_AT      0x10    // BT state block, in segment 1
#define BTC_LOCAL_ADDR_AT 0x10    // the Vita's own address, inside the state block

#define BTC_PSM_SDP       0x0001
#define BTC_PSM_RFCOMM    0x0003
#define BTC_SCID_RFCOMM   0x0080
#define BTC_SCID_SDP      0x0081
#define BTC_CHANNEL_DEFAULT 1       // where iPhones put the iAP2 service
#define BTC_SDP_WAIT_US   1500000   // open the DLC without SDP after this
#define BTC_OPEN_DELAY_US 300000    // let the link settle after it connects
#define BTC_INITIAL_CREDITS 128
#define BTC_CREDIT_BATCH  32        // hand credits back once this many frames came in
#define BTC_RING_SLOTS    16
#define BTC_CAPTURE_BYTES 1536
#define BTC_FRAME_PAYLOAD 120       // keeps RFCOMM's length field to one octet
#define BTC_WAIT_US       20000

#define BTC_FLAG_EXIT     0x1
#define BTC_FLAG_WORK     0x2

typedef int   (*BtcLockFn)(void);
typedef void  (*BtcUnlockFn)(void);
typedef void *(*BtcConnGetFn)(void *state, int type, unsigned int mac0, unsigned int mac1);
typedef int   (*BtcConnReqFn)(void *conn, void *chan, int unused, int flags);
typedef void *(*BtcChanNewFn)(void *conn, int type, unsigned int key);
typedef int   (*BtcSendFn)(void *conn, int cid, const char *format, const void *data, int length);

typedef struct BtcPdu {
    uint32_t conn;
    int32_t length;
    uint8_t data[BTC_CAPTURE_BYTES];
} BtcPdu;

static BtcLockFn g_btcLock;
static BtcUnlockFn g_btcUnlock;
static BtcConnGetFn g_btcConnGet;
static BtcConnReqFn g_btcConnReq;
static BtcChanNewFn g_btcChanNew;
static BtcSendFn g_btcSend;
static void *g_btcState;

static tai_hook_ref_t g_btcHookRef;
static SceUID g_btcHookUid = -1;
static SceUID g_btcFlags = -1;
static volatile int g_btcInstallResult; // 0, or why the client is not installed

static void *volatile g_btcConn;        // the phone's connection object
static volatile uint32_t g_btcMilestones;
static volatile uint32_t g_btcDcidRfcomm;
static volatile uint32_t g_btcDcidSdp;
static volatile uint32_t g_btcChannel = BTC_CHANNEL_DEFAULT;
static volatile uint32_t g_btcDlci;
static volatile uint32_t g_btcMuxUpUs;
static volatile int g_btcPipeUp;
static volatile int g_btcTxCredits;
static volatile int g_btcRxConsumed;
static volatile int g_btcOpenPending;
static volatile int g_btcResetPending;
static volatile uint32_t g_btcOpenMac0;
static volatile uint32_t g_btcOpenMac1;
static volatile int g_btcOpenResult;

static volatile uint32_t g_btcRxFrames;
static volatile uint32_t g_btcRxBytes;
static volatile uint32_t g_btcTxBytes;
static volatile uint32_t g_btcTruncated; // PDUs bigger than the capture slot
static volatile uint32_t g_btcRingDropped;
static volatile uint32_t g_btcLastMcc;

static BtcPdu g_btcRing[BTC_RING_SLOTS];
static volatile uint32_t g_btcRingWrite;
static volatile uint32_t g_btcRingRead;

static void btcMark(uint32_t bit)
{
    __sync_fetch_and_or(&g_btcMilestones, bit);
}

static int btcHas(uint32_t bit)
{
    return (g_btcMilestones & bit) != 0;
}

static void btcWake(void)
{
    if (g_btcFlags >= 0)
        ksceKernelSetEventFlag(g_btcFlags, BTC_FLAG_WORK);
}

// RFCOMM FCS: CRC-8, reversed polynomial 0xE0, start 0xFF, complemented.
static uint8_t btcFcs(const uint8_t *data, int length)
{
    uint8_t fcs = 0xff;
    for (int i = 0; i < length; i++) {
        fcs ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            fcs = (fcs & 1) ? (uint8_t)((fcs >> 1) ^ 0xe0) : (uint8_t)(fcs >> 1);
    }
    return (uint8_t)(0xff ^ fcs);
}

// L2CAP PDU hook: copies the frames on our channels. Filters on channel ids only.
static int btcSigHook(int a0, int a1, int a2, int a3)
{
    const uint8_t *pdu = (const uint8_t *)(uintptr_t)(unsigned int)a1;
    const int length = a2;
    if (pdu != NULL && length >= 8 && g_btcConn != NULL) {
        const unsigned int cid = pdu[6] | ((unsigned int)pdu[7] << 8);
        if (cid == 1 || cid == BTC_SCID_RFCOMM || cid == BTC_SCID_SDP) {
            const uint32_t write = g_btcRingWrite;
            if (write - g_btcRingRead >= BTC_RING_SLOTS) {
                ++g_btcRingDropped;
            } else {
                BtcPdu *slot = &g_btcRing[write % BTC_RING_SLOTS];
                int copy = length;
                if (copy > BTC_CAPTURE_BYTES) {
                    copy = BTC_CAPTURE_BYTES;
                    ++g_btcTruncated;
                }
                slot->conn = (uint32_t)a0;
                for (int i = 0; i < copy; i++)
                    slot->data[i] = pdu[i];
                slot->length = length;
                __sync_synchronize();
                g_btcRingWrite = write + 1;
                btcWake();
            }
        }
    }
    return vitaAutoHookContinue(g_btcHookRef, a0, a1, a2, a3, 0, 0);
}

static int btcSendData(uint32_t dcid, const void *data, int length)
{
    if (g_btcSend == NULL || g_btcConn == NULL || dcid == 0)
        return -1;
    return g_btcSend(g_btcConn, (int)dcid, "#", data, length);
}

static int btcSendFrame(const uint8_t *frame, int length)
{
    int result;
    if (g_btcLock() != 0)
        return -1;
    result = btcSendData(g_btcDcidRfcomm, frame, length);
    g_btcUnlock();
    return result;
}

// Opens one L2CAP channel to psm with our source id. Caller holds the BT lock.
static int btcOpenChannel(void *conn, unsigned int psm, unsigned int scid)
{
    uint8_t request[0x48] __attribute__((aligned(8)));
    uint8_t *entry = (uint8_t *)g_btcChanNew(conn, 0x14, scid);
    if (entry != NULL) {
        *(uint16_t *)(entry + 0x36) = (uint16_t)scid; // what the response handlers match on
        *(uint32_t *)(entry + 8) = 0;                 // no config options
    }
    memset(request, 0, sizeof(request));
    *(uint16_t *)(request + 0x34) = (uint16_t)psm;
    *(uint16_t *)(request + 0x36) = (uint16_t)scid;
    return g_btcConnReq(conn, request, 0, 0);
}

static void btcResetState(void)
{
    g_btcConn = NULL;
    g_btcMilestones = 0;
    g_btcDcidRfcomm = 0;
    g_btcDcidSdp = 0;
    g_btcChannel = BTC_CHANNEL_DEFAULT;
    g_btcDlci = 0;
    g_btcMuxUpUs = 0;
    g_btcPipeUp = 0;
    g_btcTxCredits = 0;
    g_btcRxConsumed = 0;
    g_btcRingRead = g_btcRingWrite;
}

static void btcOpen(uint32_t mac0, uint32_t mac1)
{
    void *conn;
    btcResetState();
    if (g_btcLock() != 0) {
        g_btcOpenResult = -1;
        return;
    }
    conn = g_btcConnGet(g_btcState, 1, mac0, mac1);
    if (conn == NULL) {
        g_btcUnlock();
        g_btcOpenResult = -2;
        return;
    }
    g_btcConn = conn;
    btcMark(VITA_AUTO_IAP2_CONN);
    g_btcOpenResult = btcOpenChannel(conn, BTC_PSM_RFCOMM, BTC_SCID_RFCOMM);
    btcOpenChannel(conn, BTC_PSM_SDP, BTC_SCID_SDP);
    g_btcUnlock();
    vitaUsbLogInfo("[VitaAutoBT] iAP2 client: dialled the phone (%d)\n", g_btcOpenResult);
}

// SABM on DLCI 0 starts the multiplexer once both RFCOMM directions are configured.
static void btcTryStartMux(void)
{
    uint8_t frame[4];
    if (btcHas(VITA_AUTO_IAP2_SABM0) || !btcHas(VITA_AUTO_IAP2_CFG_REQ) || !btcHas(VITA_AUTO_IAP2_CFG_RSP) ||
        g_btcDcidRfcomm == 0)
        return;
    frame[0] = 0x03;
    frame[1] = 0x3f;
    frame[2] = 0x01;
    frame[3] = btcFcs(frame, 3);
    if (btcSendFrame(frame, sizeof(frame)) < 0)
        return; // lock busy: try again
    btcMark(VITA_AUTO_IAP2_SABM0);
}

static void btcTrySdp(void)
{
    static const uint8_t uuid[16] = {
        0x00, 0x00, 0x00, 0x00, 0xde, 0xca, 0xfa, 0xde,
        0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xfe };
    uint8_t request[32];
    int length = 0;
    int paramsAt;
    int result;

    if (btcHas(VITA_AUTO_IAP2_SDP_SENT) || g_btcDcidSdp == 0 ||
        !btcHas(VITA_AUTO_IAP2_SDP_CFG_REQ) || !btcHas(VITA_AUTO_IAP2_SDP_CFG_RSP))
        return;
    request[length++] = 0x06; // ServiceSearchAttributeRequest
    request[length++] = 0x00;
    request[length++] = 0x01; // transaction id
    paramsAt = length;
    length += 2;              // parameter length, filled in below
    request[length++] = 0x35; request[length++] = 17; request[length++] = 0x1c;
    for (int i = 0; i < 16; i++)
        request[length++] = uuid[i];
    request[length++] = 0xff; request[length++] = 0xff; // max attribute bytes
    request[length++] = 0x35; request[length++] = 3;
    request[length++] = 0x09; request[length++] = 0x00; request[length++] = 0x04;
    request[length++] = 0x00; // no continuation
    request[paramsAt] = (uint8_t)((length - paramsAt - 2) >> 8);
    request[paramsAt + 1] = (uint8_t)((length - paramsAt - 2) & 0xff);
    if (g_btcLock() != 0)
        return;
    result = btcSendData(g_btcDcidSdp, request, length);
    g_btcUnlock();
    if (result >= 0)
        btcMark(VITA_AUTO_IAP2_SDP_SENT);
}

// The iAP2 channel follows the RFCOMM protocol UUID in the descriptor list: 0x19 0x00 0x03 0x08 <channel>.
static void btcReadSdpAnswer(const uint8_t *data, int length)
{
    uint32_t channel = 0;
    for (int i = 8; i + 4 < length; i++) {
        if (data[i] == 0x19 && data[i + 1] == 0x00 && data[i + 2] == 0x03 &&
            data[i + 3] == 0x08 && data[i + 4] >= 1 && data[i + 4] <= 30) {
            channel = data[i + 4];
            break;
        }
    }
    if (channel != 0)
        g_btcChannel = channel;
    btcMark(VITA_AUTO_IAP2_SDP_DONE);
}

static uint32_t btcAddressOctet(void)
{
    return 0x03u | (g_btcDlci << 2); // EA, command from the initiator, DLCI
}

// SABM on the iAP2 DLCI once the multiplexer is up and SDP named the channel (or had long enough to).
static void btcTryOpenDlc(void)
{
    uint8_t frame[4];
    if (btcHas(VITA_AUTO_IAP2_SABM_DLC) || !btcHas(VITA_AUTO_IAP2_UA0))
        return;
    if (!btcHas(VITA_AUTO_IAP2_SDP_DONE) &&
        (uint32_t)(ksceKernelGetSystemTimeLow() - g_btcMuxUpUs) < BTC_SDP_WAIT_US)
        return;
    g_btcDlci = g_btcChannel << 1;
    frame[0] = (uint8_t)btcAddressOctet();
    frame[1] = 0x3f;
    frame[2] = 0x01;
    frame[3] = btcFcs(frame, 3);
    if (btcSendFrame(frame, sizeof(frame)) < 0)
        return;
    btcMark(VITA_AUTO_IAP2_SABM_DLC);
}

static void btcSendMsc(int response)
{
    uint8_t frame[8] = { 0x03, 0xef, 0x09, 0xe3, 0x05, 0, 0x8d, 0 };
    frame[3] = response ? 0xe1 : 0xe3;
    frame[5] = (uint8_t)btcAddressOctet();
    frame[7] = btcFcs(frame, 2);
    if (btcSendFrame(frame, sizeof(frame)) >= 0 && !response)
        btcMark(VITA_AUTO_IAP2_MSC_SENT);
}

static void btcGrantCredits(int grant)
{
    uint8_t frame[5];
    if (grant <= 0)
        return;
    if (grant > 255)
        grant = 255;
    frame[0] = (uint8_t)btcAddressOctet();
    frame[1] = 0xff; // UIH with P/F: a credit octet follows the length
    frame[2] = 0x01; // no data
    frame[3] = (uint8_t)grant;
    frame[4] = btcFcs(frame, 2);
    if (btcSendFrame(frame, sizeof(frame)) >= 0)
        __sync_fetch_and_sub(&g_btcRxConsumed, grant);
}

// Usable once the phone has spoken on it (or answered our modem status): lend it credits so it can send first.
static void btcPipeUp(void)
{
    if (g_btcPipeUp || g_btcDlci == 0)
        return;
    g_btcPipeUp = 1;
    btcMark(VITA_AUTO_IAP2_PIPE);
    btcGrantCredits(BTC_INITIAL_CREDITS);
    btcWake();
}

static void btcHandleSignal(const BtcPdu *pdu, int length)
{
    const uint8_t *d = pdu->data;
    const unsigned int code = d[8];
    if (code == 0x03 && length >= 18) { // CONNECTION_RSP: dcid, scid, result
        const unsigned int dcid = d[12] | ((unsigned int)d[13] << 8);
        const unsigned int scid = d[14] | ((unsigned int)d[15] << 8);
        const unsigned int result = d[16] | ((unsigned int)d[17] << 8);
        if (result == 0) {
            if (scid == BTC_SCID_RFCOMM) {
                g_btcDcidRfcomm = dcid;
                btcMark(VITA_AUTO_IAP2_L2CAP_RSP);
            } else if (scid == BTC_SCID_SDP) {
                g_btcDcidSdp = dcid;
            }
        }
    } else if (code == 0x04 && length >= 14) { // CONFIG_REQ: dcid is our id
        const unsigned int dcid = d[12] | ((unsigned int)d[13] << 8);
        if (dcid == BTC_SCID_RFCOMM)
            btcMark(VITA_AUTO_IAP2_CFG_REQ);
        else if (dcid == BTC_SCID_SDP)
            btcMark(VITA_AUTO_IAP2_SDP_CFG_REQ);
    } else if (code == 0x05 && length >= 18) { // CONFIG_RSP: scid, flags, result
        const unsigned int scid = d[12] | ((unsigned int)d[13] << 8);
        const unsigned int result = d[16] | ((unsigned int)d[17] << 8);
        if (result == 0 && scid == BTC_SCID_RFCOMM)
            btcMark(VITA_AUTO_IAP2_CFG_RSP);
        else if (result == 0 && scid == BTC_SCID_SDP)
            btcMark(VITA_AUTO_IAP2_SDP_CFG_RSP);
    } else if (code == 0x06 && length >= 16) { // DISCONNECT_REQ: dcid is our id
        const unsigned int dcid = d[12] | ((unsigned int)d[13] << 8);
        if (dcid == BTC_SCID_RFCOMM) {
            btcMark(VITA_AUTO_IAP2_LOST);
            g_btcPipeUp = 0;
        }
    }
}

static void btcHandleRfcomm(const BtcPdu *pdu, int length)
{
    const uint8_t *d = pdu->data;
    const unsigned int address = d[8];
    const unsigned int control = d[9];
    const unsigned int dlci = address >> 2;
    const unsigned int type = control & ~0x10u; // without the P/F bit

    if (type == 0x63 && dlci == 0) {            // UA on DLCI 0: multiplexer up
        g_btcMuxUpUs = ksceKernelGetSystemTimeLow();
        btcMark(VITA_AUTO_IAP2_UA0);
    } else if (type == 0x63 && dlci == g_btcDlci && g_btcDlci != 0) {
        btcMark(VITA_AUTO_IAP2_UA_DLC);                  // iAP2 channel open
        btcSendMsc(0);
    } else if (type == 0x0f && dlci != 0) {     // DM: the phone refused
        btcMark(VITA_AUTO_IAP2_DM);
    } else if (type == 0xef && dlci == 0 && length >= 13) {
        const unsigned int mcc = d[11];
        g_btcLastMcc = mcc;
        if (mcc == 0xe3 && length >= 15) {
            uint8_t frame[8] = { 0x03, 0xef, 0x09, 0xe1, 0x05, 0, 0, 0 };
            frame[5] = d[13];
            frame[6] = d[14];
            frame[7] = btcFcs(frame, 2);
            btcSendFrame(frame, sizeof(frame));
            if (!btcHas(VITA_AUTO_IAP2_MSC_SENT))
                btcSendMsc(0);
        } else if (mcc == 0xe1) {
            btcMark(VITA_AUTO_IAP2_MSC_RSP);
            btcPipeUp();
        }
    } else if (type == 0xef && g_btcDlci != 0 && dlci == g_btcDlci) {
        // Data on the iAP2 channel: a one- or two-octet length, then a credit octet under credit-based flow control.
        const int hasCredit = (control & 0x10) != 0;
        int at = 10;
        unsigned int data;
        if (at >= length)
            return;
        if (d[at] & 1) {
            data = d[at] >> 1;
            at += 1;
        } else {
            if (at + 1 >= length)
                return;
            data = (d[at] >> 1) | ((unsigned int)d[at + 1] << 7);
            at += 2;
        }
        if (hasCredit) {
            if (at >= length)
                return;
            __sync_fetch_and_add(&g_btcTxCredits, d[at]);
            at += 1;
        }
        if ((int)(at + data) > length)
            data = (length > at) ? (unsigned int)(length - at) : 0;
        if (data != 0) {
            btFifoPush(&d[at], data);
            btWakeReader(BT_READ_RX);
            g_btcRxBytes += data;
        }
        ++g_btcRxFrames;
        __sync_fetch_and_add(&g_btcRxConsumed, 1);
        btcPipeUp();
    }
}

static void btcDrain(void)
{
    while (g_btcRingRead != g_btcRingWrite) {
        const BtcPdu *pdu = &g_btcRing[g_btcRingRead % BTC_RING_SLOTS];
        int length = pdu->length;
        if (length > BTC_CAPTURE_BYTES)
            length = BTC_CAPTURE_BYTES;
        if (length >= 10) {
            const unsigned int cid = pdu->data[6] | ((unsigned int)pdu->data[7] << 8);
            if (cid == 1)
                btcHandleSignal(pdu, length);
            else if (cid == BTC_SCID_RFCOMM)
                btcHandleRfcomm(pdu, length);
            else if (cid == BTC_SCID_SDP && pdu->data[8] == 0x07)
                btcReadSdpAnswer(pdu->data, length);
        }
        __sync_synchronize();
        ++g_btcRingRead;
    }
}

static int btcThreadMain(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        unsigned int out = 0;
        SceUInt timeout = BTC_WAIT_US;
        ksceKernelWaitEventFlag(g_btcFlags, BTC_FLAG_EXIT | BTC_FLAG_WORK,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &out, &timeout);
        if (out & BTC_FLAG_EXIT)
            break;
        if (g_btcResetPending) {
            g_btcResetPending = 0;
            btcResetState();
        }
        if (g_btcOpenPending) {
            const uint32_t mac0 = g_btcOpenMac0;
            const uint32_t mac1 = g_btcOpenMac1;
            g_btcOpenPending = 0;
            ksceKernelDelayThread(BTC_OPEN_DELAY_US);
            btcOpen(mac0, mac1);
            continue;
        }
        if (g_btcConn == NULL)
            continue;
        btcDrain();
        btcTryStartMux();
        btcTrySdp();
        btcTryOpenDlc();
        // The phone's credits run out if we never write: give them back.
        if (g_btcPipeUp && g_btcRxConsumed >= BTC_CREDIT_BATCH)
            btcGrantCredits(g_btcRxConsumed);
    }
    return 0;
}

// The phone connected: dial it back. Runs in the Bluetooth worker, so it only records the request.
static void btcRequestOpen(uint32_t mac0, uint32_t mac1)
{
    g_btcOpenMac0 = mac0;
    g_btcOpenMac1 = mac1;
    __sync_synchronize();
    g_btcOpenPending = 1;
    btcWake();
}

static void btcReset(void)
{
    g_btcPipeUp = 0;
    g_btcResetPending = 1;
    btcWake();
}

// Bytes from the app to the phone as UIH data. Returns the bytes taken: fewer (or 0) when out of credits.
static int btcWrite(const void *userBuffer, uint32_t length)
{
    uint32_t done = 0;
    if (!g_btcPipeUp || g_btcConn == NULL || g_btcDcidRfcomm == 0 || g_btcDlci == 0)
        return -1;
    while (done < length) {
        uint8_t chunk[BTC_FRAME_PAYLOAD];
        uint8_t frame[BTC_FRAME_PAYLOAD + 5];
        uint32_t count = length - done;
        int grant;
        if (g_btcTxCredits <= 0)
            break;
        if (count > BTC_FRAME_PAYLOAD)
            count = BTC_FRAME_PAYLOAD;
        if (ksceKernelMemcpyUserToKernel(chunk, (const uint8_t *)userBuffer + done, count) < 0)
            return done != 0 ? (int)done : -1;
        grant = g_btcRxConsumed;
        if (grant > 200)
            grant = 200;
        if (grant < 0)
            grant = 0;
        frame[0] = (uint8_t)btcAddressOctet();
        frame[1] = 0xff;
        frame[2] = (uint8_t)((count << 1) | 1);
        frame[3] = (uint8_t)grant;
        memcpy(&frame[4], chunk, count);
        frame[4 + count] = btcFcs(frame, 2);
        if (btcSendFrame(frame, (int)(count + 5)) < 0)
            break;
        __sync_fetch_and_sub(&g_btcRxConsumed, grant);
        __sync_fetch_and_sub(&g_btcTxCredits, 1);
        g_btcTxBytes += count;
        done += count;
    }
    return (int)done;
}

static int btcInstall(void)
{
    SceKernelModuleInfo info;
    SceUID modid;
    uintptr_t seg0;
    uintptr_t seg1;

    g_btcInstallResult = 0;
    if (g_btSearchResult != 0)
        return g_btcInstallResult = -1; // SceBt is not the firmware these offsets are for
    if (btFindSceBt(&info, &modid) < 0)
        return g_btcInstallResult = -2;
    seg0 = (uintptr_t)info.segments[0].vaddr;
    seg1 = (uintptr_t)info.segments[1].vaddr;
    g_btcLock = (BtcLockFn)((seg0 + BTC_LOCK_AT) | 1);
    g_btcUnlock = (BtcUnlockFn)((seg0 + BTC_UNLOCK_AT) | 1);
    g_btcConnGet = (BtcConnGetFn)((seg0 + BTC_CONN_GET_AT) | 1);
    g_btcConnReq = (BtcConnReqFn)((seg0 + BTC_CONN_REQ_AT) | 1);
    g_btcChanNew = (BtcChanNewFn)((seg0 + BTC_CHAN_NEW_AT) | 1);
    g_btcSend = (BtcSendFn)((seg0 + BTC_SEND_AT) | 1);
    g_btcState = (void *)(seg1 + BTC_STATE_AT);

    btcResetState();
    g_btcOpenPending = 0;
    g_btcResetPending = 0;
    g_btcRxFrames = 0;
    g_btcRxBytes = 0;
    g_btcTxBytes = 0;
    g_btcTruncated = 0;
    g_btcRingDropped = 0;
    g_btcLastMcc = 0;
    g_btcOpenResult = 0;

    g_btcFlags = ksceKernelCreateEventFlag("vita_auto_btc", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    if (g_btcFlags < 0) {
        const int result = g_btcFlags;
        g_btcFlags = -1;
        return g_btcInstallResult = result;
    }
    g_btcHookUid = taiHookFunctionOffsetForKernel(KERNEL_PID, &g_btcHookRef, modid, 0,
        BTC_SIG_AT, 1, (const void *)btcSigHook);
    if (g_btcHookUid < 0) {
        const int result = g_btcHookUid;
        g_btcHookUid = -1;
        ksceKernelDeleteEventFlag(g_btcFlags);
        g_btcFlags = -1;
        return g_btcInstallResult = result;
    }
    g_btcThread = ksceKernelCreateThread("vita_auto_btc", btcThreadMain, 0x3C, 0x4000, 0, 0x10000, 0);
    if (g_btcThread < 0) {
        const int result = g_btcThread;
        g_btcThread = -1;
        taiHookReleaseForKernel(g_btcHookUid, g_btcHookRef);
        g_btcHookUid = -1;
        ksceKernelDeleteEventFlag(g_btcFlags);
        g_btcFlags = -1;
        return g_btcInstallResult = result;
    }
    g_btcEnabled = 1;
    ksceKernelStartThread(g_btcThread, 0, NULL);
    return 0;
}

// No link close: sending DISC here crashed app exit. SceBt tears the channels down.
static void btcRemove(void)
{
    g_btcEnabled = 0;
    if (g_btcThread >= 0) {
        ksceKernelSetEventFlag(g_btcFlags, BTC_FLAG_EXIT);
        ksceKernelWaitThreadEnd(g_btcThread, NULL, NULL);
        ksceKernelDeleteThread(g_btcThread);
        g_btcThread = -1;
    }
    if (g_btcHookUid >= 0) {
        taiHookReleaseForKernel(g_btcHookUid, g_btcHookRef);
        g_btcHookUid = -1;
    }
    if (g_btcFlags >= 0) {
        ksceKernelDeleteEventFlag(g_btcFlags);
        g_btcFlags = -1;
    }
    g_btcConn = NULL;
    g_btcPipeUp = 0;
    g_btcMilestones = 0;
}

static void btcFillInfo(VitaAutoBtInfo *info)
{
    info->iap2Installed = g_btcEnabled;
    info->iap2InstallResult = g_btcInstallResult;
    info->iap2Milestones = g_btcMilestones;
    info->iap2OpenResult = g_btcOpenResult;
    info->iap2Channel = g_btcChannel;
    info->iap2TxCredits = g_btcTxCredits;
    info->iap2RxFrames = g_btcRxFrames;
    info->iap2RxBytes = g_btcRxBytes;
    info->iap2TxBytes = g_btcTxBytes;
    info->iap2Truncated = g_btcTruncated;
    info->iap2RingDropped = g_btcRingDropped;
    info->iap2LastMcc = g_btcLastMcc;
    info->iap2LocalMac0 = 0;
    info->iap2LocalMac1 = 0;
    if (g_btcEnabled && g_btcState != NULL) {
        const volatile uint32_t *local = (const volatile uint32_t *)((uintptr_t)g_btcState + BTC_LOCAL_ADDR_AT);
        info->iap2LocalMac0 = local[0];
        info->iap2LocalMac1 = local[1] & 0xFFFF;
    }
}
