#pragma once

#include "app/app_errors.h"

void vitaLogReset(void);
void vitaLog(const char* format, ...);

// detail is the original SDK/USB return code, or zero when none is available.
void vitaLogAppError(app::AppError error, int detail = 0);

// VITA_AUTO_VERBOSE_LOGGING
#if VITA_AUTO_VERBOSE_LOGGING
#define vitaLogInfo(...) vitaLog(__VA_ARGS__)
#else
#define vitaLogInfo(...) ((void)0)
#endif
