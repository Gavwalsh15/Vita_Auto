#include <psp2/kernel/processmgr.h>

#include <mbedtls/platform_time.h>

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(sceKernelGetProcessTimeWide() / 1000);
}
