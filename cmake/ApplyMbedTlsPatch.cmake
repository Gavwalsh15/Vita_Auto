# Build-tree variant: the pinned Mbed TLS source archive does not ship a .git
# directory, so the upstream script's git-based patch check cannot run here.
# The legacy UTCTime compatibility patch has already been applied to
# third_party/mbedtls/library/x509.c in this build tree. Verify it is present
# instead of invoking Git.

set(_mbedtls_x509 "${VITA_AUTO_MBEDTLS_DIR}/library/x509.c")
if(NOT EXISTS "${_mbedtls_x509}")
    message(FATAL_ERROR "Mbed TLS x509.c not found at ${_mbedtls_x509}")
endif()

file(READ "${_mbedtls_x509}" _mbedtls_x509_source)
if(NOT _mbedtls_x509_source MATCHES "legacy Google Automotive Link")
    message(FATAL_ERROR
        "Mbed TLS legacy certificate patch is not applied to ${_mbedtls_x509}")
endif()

message(STATUS "Mbed TLS legacy certificate patch is already applied")
