#pragma once

#include "vita_auto_usb.h"

// Kernel syscalls imported by the SUPRX. muststay compatible with
// ../exports.yml and the public structs above

int ksceVitaAutoUsbGetState(void);
int ksceVitaAutoUsbReadWaitTimed(
    void *buffer,
    uint32_t capacity,
    uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *metadata
);
int ksceVitaAutoUsbWake(void);
int ksceVitaAutoUsbWrite(const void *buffer, uint32_t length);
int ksceVitaAutoUsbGetAccessoryActivateResult(void);
int ksceVitaAutoUsbGetHostModeResult(void);
int ksceVitaAutoUsbGetDriverRegisterResult(void);
unsigned int ksceVitaAutoUsbGetLastVidPid(void);
int ksceVitaAutoUsbGetLastFailureStage(void);
int ksceVitaAutoUsbGetLastFailureResult(void);
int ksceVitaAutoUsbGetLastFailureCount(void);
int ksceVitaAutoUsbStart(void);
int ksceVitaAutoUsbStop(void);
int ksceVitaAutoUsbDrainReads(void);
int ksceVitaAutoUsbGetCurrentHostMode(void);
int ksceVitaAutoUsbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *diagnostics);
