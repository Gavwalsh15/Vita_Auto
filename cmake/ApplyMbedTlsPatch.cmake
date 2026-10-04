# Keep the Android Auto certificate compatibility fix reproducible after a
# fresh submodule checkout. An already-patched local tree is left alone.
find_package(Git REQUIRED)
set(_mbedtls_patch
    "${CMAKE_CURRENT_LIST_DIR}/../patches/mbedtls-x509-legacy-timezone.patch")
# Also support source archives under the main repository: Git must not use
# the parent repository and silently skip paths relative to Mbed TLS.
get_filename_component(_mbedtls_parent "${VITA_AUTO_MBEDTLS_DIR}" DIRECTORY)
get_filename_component(_mbedtls_name "${VITA_AUTO_MBEDTLS_DIR}" NAME)
# Run outside the submodule so copied sources with a stale .git pointer work too.
set(_mbedtls_git "${CMAKE_COMMAND}" -E env
    "GIT_CEILING_DIRECTORIES=${_mbedtls_parent}" "${GIT_EXECUTABLE}")

execute_process(
    COMMAND ${_mbedtls_git} apply "--directory=${_mbedtls_name}"
            --reverse --check --ignore-space-change
            "${_mbedtls_patch}"
    WORKING_DIRECTORY "${_mbedtls_parent}"
    RESULT_VARIABLE _patch_present
    OUTPUT_QUIET ERROR_QUIET
)
if(_patch_present EQUAL 0)
    message(STATUS "Mbed TLS legacy certificate patch is already applied")
else()
    execute_process(
        COMMAND ${_mbedtls_git} apply "--directory=${_mbedtls_name}"
                --check --ignore-space-change
                "${_mbedtls_patch}"
        WORKING_DIRECTORY "${_mbedtls_parent}"
        RESULT_VARIABLE _patch_check
        ERROR_VARIABLE _patch_error
    )
    if(NOT _patch_check EQUAL 0)
        message(FATAL_ERROR
            "Cannot apply the Mbed TLS legacy certificate patch. "
            "Check the pinned submodule revision and local x509.c edits.\n"
            "${_patch_error}")
    endif()
    execute_process(
        COMMAND ${_mbedtls_git} apply "--directory=${_mbedtls_name}"
                --ignore-space-change "${_mbedtls_patch}"
        WORKING_DIRECTORY "${_mbedtls_parent}"
        RESULT_VARIABLE _patch_result
        ERROR_VARIABLE _patch_error
    )
    if(NOT _patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to patch Mbed TLS:\n${_patch_error}")
    endif()
    message(STATUS "Applied Mbed TLS legacy certificate patch")
endif()
