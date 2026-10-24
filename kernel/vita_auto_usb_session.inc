// Accessory configuration and Android Auto version handshake.

static void handleAccessoryConfigurationDone(int32_t result, int32_t count, void *arg)
{
    (void)arg;

    g_accessoryConfigPending = 0;

    if (g_usb.state == USB_IDLE)
        return;

    if (result < 0) {
        ksceDebugPrintf(
            "[VitaAutoUSB] SET_CONFIGURATION failed: result=0x%08x count=%d\n",
            result,
            count
        );
        setUsbFailure(VITA_AUTO_USB_FAILURE_CONFIGURATION_TRANSFER, result, count);
        return;
    }

    g_accessoryConfigured = 1;
    g_versionSettlePolls = AA_CONFIG_SETTLE_POLLS;
    vitaUsbLogInfo("[VitaAutoUSB] Accessory configuration active\n");
}

static int openAccessoryBulkPipes(int deviceId)
{
    SceUsbdEndpointDescriptor *endpoint = NULL;

    while ((endpoint = ksceUsbdScanStaticDescriptor(
                deviceId,
                endpoint,
                SCE_USBD_DESCRIPTOR_ENDPOINT
            )) != NULL) {
        if ((endpoint->bmAttributes & SCE_USBD_ENDPOINT_TRANSFER_TYPE_BITS) !=
            SCE_USBD_ENDPOINT_TRANSFER_TYPE_BULK) {
            continue;
        }

        vitaUsbLogInfo(
            "[VitaAutoUSB] USB bulk endpoint address=0x%02x packet=%u\n",
            endpoint->bEndpointAddress,
            endpoint->wMaxPacketSize
        );

        if ((endpoint->bEndpointAddress & SCE_USBD_ENDPOINT_DIRECTION_BITS) ==
            SCE_USBD_ENDPOINT_DIRECTION_IN) {
            if (g_usb.bulkInPipe < 0) {
                g_usb.bulkInPipe = ksceUsbdOpenPipe(deviceId, endpoint);
                g_bulkEndpointPacketSize =
                    endpoint->wMaxPacketSize & USB_ENDPOINT_PACKET_SIZE_MASK;
                if (g_bulkEndpointPacketSize == 0 ||
                    g_bulkEndpointPacketSize > USB_BULK_MAX_PACKET_SIZE)
                {
                    g_bulkEndpointPacketSize = USB_BULK_MAX_PACKET_SIZE;
                }
            }
        } else if (g_usb.bulkOutPipe < 0) {
            g_usb.bulkOutPipe = ksceUsbdOpenPipe(deviceId, endpoint);
        }
    }

    if (g_usb.bulkInPipe < 0 || g_usb.bulkOutPipe < 0) {
        ksceDebugPrintf(
            "[VitaAutoUSB] Could not open USB bulk endpoints: in=0x%08x out=0x%08x\n",
            g_usb.bulkInPipe,
            g_usb.bulkOutPipe
        );
        return -1;
    }

    vitaUsbLogInfo(
        "[VitaAutoUSB] USB bulk endpoints opened read-packet=%u\n",
        g_bulkEndpointPacketSize
    );
    return 0;
}

static int startAndroidAutoVersionHandshake(int deviceId)
{
    const SceUsbdConfigurationDescriptor *configuration;
    int submitResult;

    g_usb.state = USB_OPENING_BULK;

    configuration = ksceUsbdScanStaticDescriptor(
        deviceId,
        NULL,
        SCE_USBD_DESCRIPTOR_CONFIGURATION
    );
    if (configuration == NULL || configuration->bConfigurationValue == 0) {
        ksceDebugPrintf("[VitaAutoUSB] USB configuration descriptor missing\n");
        setUsbFailure(VITA_AUTO_USB_FAILURE_ACCESSORY_CONFIGURATION_SETUP, -1, 0);
        return -1;
    }

    /*
     Keep this pipe separate from the Samsung identity's EP0 pipe. Its
     delayed detach callback can arrive after 18d1:2d00 has attached.
     */
    g_accessoryControlPipe = ksceUsbdOpenPipe(deviceId, NULL);
    if (g_accessoryControlPipe < 0) {
        ksceDebugPrintf(
            "[VitaAutoUSB] Could not open USB control pipe: 0x%08x\n",
            g_accessoryControlPipe
        );
        setUsbFailure(
            VITA_AUTO_USB_FAILURE_ACCESSORY_CONFIGURATION_SETUP,
            g_accessoryControlPipe,
            0
        );
        return -1;
    }

    if (openAccessoryBulkPipes(deviceId) < 0) {
        setUsbFailure(VITA_AUTO_USB_FAILURE_BULK_PIPE_OPEN, -1, 0);
        return -1;
    }

    /*
     Do not start bulk I/O from inside SceUsbd's attach callback. Let the
     accessory configuration settle, then mirror the proven PC sequence:
     write VERSION_REQUEST first and arm the IN read after write completion.
     */
    g_versionSettlePolls = 0;
    g_versionWriteCompleted = 0;
    g_versionRetryCount = 0;
    g_accessoryConfigPending = 1;
    g_accessoryConfigured = 0;

    g_accessoryConfigRequest.bmRequestType = 0x00;
    g_accessoryConfigRequest.bRequest = SCE_USBD_REQUEST_SET_CONFIGURATION;
    g_accessoryConfigRequest.wValue = configuration->bConfigurationValue;
    g_accessoryConfigRequest.wIndex = 0;
    g_accessoryConfigRequest.wLength = 0;

    vitaUsbLogInfo(
        "[VitaAutoUSB] Setting USB configuration %u interfaces=%u\n",
        configuration->bConfigurationValue,
        configuration->bNumInterfaces
    );

    submitResult = queueControlTransfer(
        g_accessoryControlPipe,
        &g_accessoryConfigRequest,
        NULL,
        accessoryConfigurationDone,
        NULL
    );
    if (submitResult < 0) {
        g_accessoryConfigPending = 0;
        ksceDebugPrintf(
            "[VitaAutoUSB] Could not submit SET_CONFIGURATION: 0x%08x\n",
            submitResult
        );
        setUsbFailure(VITA_AUTO_USB_FAILURE_CONFIGURATION_SUBMIT, submitResult, 0);
        return -1;
    }

    return 0;
}

static void pumpAndroidAutoVersionHandshake(void)
{
    if (g_usb.state == USB_GETTING_PROTOCOL && g_protocolRequestPending) {
        int submitResult;

        if (g_protocolSettlePolls > 0) {
            --g_protocolSettlePolls;
            return;
        }
        if (g_usb.controlPipe < 0)
            return;

        g_protocolRequestPending = 0;
        submitResult = submitControlRequest(
            USB_REQ_TO_HOST_VENDOR,
            USB_GET_PROTOCOL,
            0,
            0,
            sizeof(g_usb.protocol),
            g_usb.protocol
        );
        if (submitResult < 0)
            setUsbFailure(
                VITA_AUTO_USB_FAILURE_PROTOCOL_SUBMIT,
                submitResult,
                g_protocolRetryCount
            );
        return;
    }

    if (g_usb.state == USB_OPENING_BULK) {
        int submitResult;

        if (g_accessoryConfigPending || !g_accessoryConfigured)
            return;

        if (g_versionSettlePolls > 0) {
            --g_versionSettlePolls;
            return;
        }

        if (g_usb.bulkOutPipe < 0 || g_bulkShared->writeBusy)
            return;

        g_usb.state = USB_AA_VERSION_SENT;
        g_bulkShared->writeBusy = 1;
        g_bulkShared->writeExpectedLength = sizeof(g_versionRequest);
        ++g_bulkShared->writeSubmitCount;
        ksceKernelDcacheCleanRange(g_versionRequest, sizeof(g_versionRequest));
        submitResult = queueBulkTransfer(
            g_usb.bulkOutPipe,
            g_versionRequest,
            sizeof(g_versionRequest),
            aaBulkWriteDone,
            NULL
        );
        if (submitResult < 0) {
            g_bulkShared->writeBusy = 0;
            ksceDebugPrintf(
                "[VitaAutoUSB] Could not submit AA version write: 0x%08x\n",
                submitResult
            );
            setUsbFailure(VITA_AUTO_USB_FAILURE_VERSION_WRITE_SUBMIT, submitResult, 0);
        }
        return;
    }

    if (g_usb.state == USB_AA_VERSION_SENT &&
        g_versionWriteCompleted &&
        !g_bulkShared->readArmed) {
        g_versionWriteCompleted = 0;
        if (submitBulkRead() < 0) {
            ksceDebugPrintf("[VitaAutoUSB] Could not submit AA bulk read\n");
            setUsbFailure(VITA_AUTO_USB_FAILURE_BULK_READ_SUBMIT, -1, 0);
        }
    }
}

// Device callbacks and module lifecycle.

static int handleUsbProbe(int deviceId)
{
    const SceUsbdDeviceDescriptor *device = ksceUsbdScanStaticDescriptor(
        deviceId,
        NULL,
        SCE_USBD_DESCRIPTOR_DEVICE
    );

    if (device != NULL) {
        g_lastVidPid = ((unsigned int)device->idVendor << 16) |
                       device->idProduct;
    }

    if (device == NULL ||
        (!isAccessoryDevice(device) && !isSupportedAndroidVendor(device->idVendor)))
        return SCE_USBD_PROBE_FAILED;

    /*
     One phone at a time, except for the expected USB re-enumeration. The
     Google accessory identity may be assigned a new SceUsbd device ID
     before the Samsung identity's detach callback arrives.
     */
    if (g_usb.deviceId >= 0 && g_usb.deviceId != deviceId &&
        !(g_waitingForAccessory && isAccessoryDevice(device))) {
        return SCE_USBD_PROBE_FAILED;
    }

    vitaUsbLogInfo(
        "[VitaAutoUSB] Candidate USB device %04x:%04x\n",
        device->idVendor,
        device->idProduct
    );
    g_lastFailureStage = 0;
    g_lastFailureResult = 0;
    g_lastFailureCount = 0;
    return SCE_USBD_PROBE_SUCCEEDED;
}

static int handleUsbAttach(int deviceId)
{
    const SceUsbdDeviceDescriptor *device = ksceUsbdScanStaticDescriptor(
        deviceId,
        NULL,
        SCE_USBD_DESCRIPTOR_DEVICE
    );

    g_usb.deviceId = deviceId;

    if (device != NULL && isAccessoryDevice(device)) {
        g_staleAccessory = !g_waitingForAccessory && g_usb.state == USB_IDLE;
        g_accessoryDeviceId = deviceId;
        g_waitingForAccessory = 0;
        if (startAndroidAutoVersionHandshake(deviceId) < 0) {
            if (g_usb.state != USB_FAILED)
                setUsbFailure(VITA_AUTO_USB_FAILURE_BULK_PIPE_OPEN, -1, 0);
            return SCE_USBD_ATTACH_FAILED;
        }
        return SCE_USBD_ATTACH_SUCCEEDED;
    }

    g_normalDeviceId = deviceId;
    g_accessoryDeviceId = -1;
    g_waitingForAccessory = 0;

    g_usb.controlPipe = ksceUsbdOpenPipe(deviceId, NULL);

    if (g_usb.controlPipe < 0) {
        ksceDebugPrintf("[VitaAutoUSB] Could not open control pipe: 0x%08x\n", g_usb.controlPipe);
        setUsbFailure(VITA_AUTO_USB_FAILURE_CONTROL_PIPE_OPEN, g_usb.controlPipe, 0);
        return SCE_USBD_ATTACH_FAILED;
    }

    g_protocolRetryCount = 0;
    g_protocolSettlePolls = USB_PROTOCOL_SETTLE_POLLS;
    __sync_synchronize();
    g_protocolRequestPending = 1;
    g_usb.state = USB_GETTING_PROTOCOL;

    return SCE_USBD_ATTACH_SUCCEEDED;
}

static int handleUsbDetach(int deviceId)
{
    // USBD has already invalidated this device's pipes before notifying us.
    if (deviceId == g_normalDeviceId) {
        __sync_lock_test_and_set(&g_usb.controlPipe, -1);
        g_normalDeviceId = -1;
        if (g_waitingForAccessory || g_accessoryDeviceId >= 0)
            return SCE_USBD_DETACH_SUCCEEDED;
    }

    if (deviceId == g_accessoryDeviceId || deviceId == g_usb.deviceId) {
        if (sessionIsOpen()) {
            g_lastFailureStage = VITA_AUTO_USB_FAILURE_DETACHED;
            g_lastFailureResult = 0;
            g_lastFailureCount = (int)g_usb.state;
        }
        retireSession(USB_IDLE);
        __sync_lock_test_and_set(&g_accessoryControlPipe, -1);
        __sync_lock_test_and_set(&g_usb.bulkInPipe, -1);
        __sync_lock_test_and_set(&g_usb.bulkOutPipe, -1);
        g_accessoryDeviceId = -1;
        g_usb.deviceId = -1;
        vitaUsbLogInfo("[VitaAutoUSB] Phone detached; request storage retained\n");
    }
    return SCE_USBD_DETACH_SUCCEEDED;
}

static int usbProbe(int deviceId)
{
    int result;
    if (!enterOperation(0))
        return SCE_USBD_PROBE_FAILED;
    result = handleUsbProbe(deviceId);
    leaveOperation();
    return result;
}

static int usbAttach(int deviceId)
{
    int result;
    if (!enterOperation(0))
        return SCE_USBD_ATTACH_FAILED;
    result = handleUsbAttach(deviceId);
    leaveOperation();
    return result;
}

static int usbDetach(int deviceId)
{
    int result;
    /* A transfer error can close admission before USBD delivers detach.
     Detach must still release its pipes. Unregister owns callback lifetime;
     count this callback even during Stop/module unload so it can be drained. */
    __sync_add_and_fetch(&g_operationGate, 1);
    result = handleUsbDetach(deviceId);
    leaveOperation();
    return result;
}

static const SceUsbdDriver g_usbDriver = {
    .name = "vita_auto_usb",
    .probe = usbProbe,
    .attach = usbAttach,
    .detach = usbDetach
};

/* Lifecycle operations run only on module/app threads, never USBD callbacks.
 A failed close/unregister/drain retains ownership and memory for a later retry. */
static int closePipe(SceUID *pipe)
{
    int result;
    /* Detach and Stop may both reach cleanup. Taking the handle first prevents
     a second close, including from a callback invoked inline by ClosePipe. */
    const SceUID ownedPipe = __sync_lock_test_and_set(pipe, -1);
    if (ownedPipe < 0)
        return 0;
    result = ksceUsbdClosePipe(ownedPipe);
    const int attached = pipe == &g_usb.controlPipe
        ? g_normalDeviceId >= 0 : g_accessoryDeviceId >= 0;
    if (result == (int)SCE_USBD_ERROR_PIPE_NOT_FOUND ||
        (!attached && result == (int)SCE_USBD_ERROR_INVALID_PARAM))
        result = 0;
    // Detach can run inside ClosePipe. Never resurrect its retired handle.
    if (result < 0 && attached)
        __sync_bool_compare_and_swap(pipe, -1, ownedPipe);
    return result;
}

static int closeSessionPipes(void)
{
    SceUID *pipes[] = {
        &g_usb.controlPipe, &g_accessoryControlPipe,
        &g_usb.bulkInPipe, &g_usb.bulkOutPipe
    };
    int firstError = 0;
    unsigned int i;
    for (i = 0; i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
        const int result = closePipe(pipes[i]);
        if (result < 0 && firstError == 0) {
            firstError = result;
            g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_CLOSE_PIPE;
            g_lastFailureResult = result;
            g_lastFailureCount = (int)i;
        }
    }
    return firstError;
}

static int waitForSessionUsers(int includeTransfers)
{
    const uint64_t startedAt = ksceKernelGetSystemTimeWide();
    while ((__sync_fetch_and_add(&g_operationGate, 0) & USB_OPERATION_COUNT) ||
           (includeTransfers && __sync_fetch_and_add(&g_pendingTransfers, 0))) {
        if (ksceKernelGetSystemTimeWide() - startedAt >= USB_STOP_TIMEOUT_US) {
            ksceDebugPrintf("[VitaAutoUSB] Stop timed out; retaining session resources\n");
            return -1;
        }
        ksceKernelDelayThread(USB_STOP_POLL_US);
    }
    return 0;
}

static void resetSession(void)
{
    g_usb.deviceId = -1;
    g_usb.stringIndex = 0;
    g_usb.state = USB_IDLE;
    g_retiredState = USB_IDLE;
    g_normalDeviceId = -1;
    g_accessoryDeviceId = -1;
    g_waitingForAccessory = 0;
    g_versionSettlePolls = 0;
    g_versionWriteCompleted = 0;
    g_versionRetryCount = 0;
    g_protocolRequestPending = 0;
    g_protocolSettlePolls = 0;
    g_protocolRetryCount = 0;
    g_staleAccessory = 0;
    g_accessoryConfigPending = 0;
    g_accessoryConfigured = 0;
    g_bulkEndpointPacketSize = USB_BULK_MAX_PACKET_SIZE;
    g_lastVidPid = 0;
    g_lastFailureStage = VITA_AUTO_USB_FAILURE_NONE;
    g_lastFailureResult = 0;
    g_lastFailureCount = 0;
    resetBulkTransfers();
    ksceKernelClearEventFlag(g_readEvent, 0);
}

static int stopSession(void)
{
    int result;
    retireSession(USB_IDLE);

    // No callback can start new work after admission closes.
    if (waitForSessionUsers(0) < 0) {
        g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_OPERATIONS;
        g_lastFailureResult = -1;
        g_lastFailureCount = (int)(g_operationGate & USB_OPERATION_COUNT);
        return -1;
    }
    // Closing a pipe can strand requests without completing their callbacks.
    // Normal disconnect drains reads while AA is still responding, before Stop.
    if (waitForSessionUsers(1) < 0) {
        g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_TRANSFERS;
        g_lastFailureResult = -1;
        g_lastFailureCount = g_pendingTransfers;
        return -1;
    }
    result = closeSessionPipes();
    if (result < 0 && g_driverRegistered) {
        /* A disconnected device can leave a pipe that ClosePipe will not
         accept. Unregister may run detach cleanup; retry the remaining
         handles before deciding that the session must stay retained. */
        const int unregisterResult = ksceUsbdUnregisterDriver(&g_usbDriver);
        if (unregisterResult >= 0) {
            g_driverRegistered = 0;
            result = closeSessionPipes();
        }
    }
    if (result < 0)
        return result;

    /* No requests remain; unregister before releasing the host role. */
    if (g_driverRegistered) {
        result = ksceUsbdUnregisterDriver(&g_usbDriver);
        if (result < 0) {
            g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_UNREGISTER;
            g_lastFailureResult = result;
            g_lastFailureCount = 0;
            return result;
        }
        g_driverRegistered = 0;
    }
    // Keep the USB role and request storage intact until callbacks drain.
    if (waitForSessionUsers(1) < 0) {
        g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_TRANSFERS;
        g_lastFailureResult = -1;
        g_lastFailureCount = g_pendingTransfers;
        return -1;
    }
    if (g_bulkShared != NULL) {
        g_bulkShared->readArmed = 0;
        g_bulkShared->writeBusy = 0;
    }
    if (g_hostModeChanged) {
        result = ksceUsbServMacSelect(USB_HOST_PORT, g_previousHostMode);
        if (result < 0) {
            g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_HOST_MODE;
            g_lastFailureResult = result;
            g_lastFailureCount = g_previousHostMode;
            return result;
        }
        g_hostModeChanged = 0;
    }
    if (g_accessoryActivated) {
        result = ksceUsbServAccessoryDeactivate();
        if (result < 0) {
            g_lastFailureStage = VITA_AUTO_USB_FAILURE_STOP_ACCESSORY;
            g_lastFailureResult = result;
            g_lastFailureCount = 0;
            return result;
        }
        g_accessoryActivated = 0;
    }
    return 0;
}

