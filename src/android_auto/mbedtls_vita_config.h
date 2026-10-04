#pragma once

// Mbed TLS defaults assume an operating-system and POSIX/Win32
// time functions. Vita supplies cryptographic random bytes directly instead;
// aa_tls.cpp seeds the DRBG from sceKernelGetRandomNumber.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_NET_C

// mbedtls_vita_platform.c supplies the monotonic milliseconds.
#define MBEDTLS_PLATFORM_MS_TIME_ALT
