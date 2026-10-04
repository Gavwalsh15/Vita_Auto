#pragma once

#include <psp2kern/kernel/debug.h>

/* Failure/warning calls use ksceDebugPrintf directly and remain in every build.
    disabled informational calls must not evaluate their arguments. */
#if VITA_AUTO_VERBOSE_LOGGING
#define vitaUsbLogInfo(...) ksceDebugPrintf(__VA_ARGS__)
#else
#define vitaUsbLogInfo(...) ((void)0)
#endif
