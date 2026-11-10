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

int ksceVitaAutoBtStart(void);
int ksceVitaAutoBtStop(void);
int ksceVitaAutoBtReadWaitTimed(void *buffer, uint32_t capacity, uint32_t timeoutUs);
int ksceVitaAutoBtWrite(const void *buffer, uint32_t length);
int ksceVitaAutoBtGetInfo(VitaAutoBtInfo *info);
int ksceVitaAutoBtEnable(void);
int ksceVitaAutoBtDisable(void);
int ksceVitaAutoBtReadEvents(VitaAutoBtEvent *events, uint32_t maxEvents);
int ksceVitaAutoBtConfigureName(const char *wanted, const char *recoverOriginal);
int ksceVitaAutoBtGetOriginalName(char *buffer, uint32_t size);
int ksceVitaAutoBtConnectState(uint32_t mac0, uint32_t mac1);
int ksceVitaAutoBtDisconnect(uint32_t mac0, uint32_t mac1);
int ksceVitaAutoBtSetOptions(uint32_t flags);

int ksceVitaAutoApStart(const VitaAutoApConfig *config);
int ksceVitaAutoApStop(void);
int ksceVitaAutoApKeepAlive(void);
int ksceVitaAutoApGetInfo(VitaAutoApInfo *info);
