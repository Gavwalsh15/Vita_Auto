cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK} OR "$ENV{VITASDK}" STREQUAL "")
    message(FATAL_ERROR
        "VITASDK is not set. Open a VitaSDK shell or set VITASDK to the "
        "folder containing share/vita.cmake before configuring.")
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

project(vita_auto LANGUAGES C CXX)

option(VITA_AUTO_VERBOSE_LOGGING "Include non-critical startup/runtime logging" OFF)
option(VITA_AUTO_PERFORMANCE "Include performance counters and reports" OFF)

set(VITA_APP_NAME "Vita Auto")
set(VITA_TITLEID "VITAUTO01")
set(VITA_VERSION "01.00")

set(VITA_AUTO_MBEDTLS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls"
    CACHE PATH "Path to the pinned Mbed TLS source tree")

if(NOT EXISTS "${VITA_AUTO_MBEDTLS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Mbed TLS is required at ${VITA_AUTO_MBEDTLS_DIR}. "
        "Add the pinned dependency under third_party/mbedtls or set "
        "VITA_AUTO_MBEDTLS_DIR explicitly.")
endif()

include(cmake/ApplyMbedTlsPatch.cmake)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/android_auto/mbedtls_vita_config.h"
    CACHE FILEPATH "" FORCE)
add_subdirectory("${VITA_AUTO_MBEDTLS_DIR}" mbedtls EXCLUDE_FROM_ALL)

add_subdirectory(kernel)

add_executable(vita_auto
    src/main.cpp

    src/app/application.cpp
    src/app/audio_settings.cpp
    src/app/connection_screen.cpp
    src/app/direct_session.cpp
    src/app/usb_shell_lock.cpp

    src/android_auto/aa_direct.cpp
    src/android_auto/aa_direct_media.cpp
    src/android_auto/aa_direct_messages.cpp
    src/android_auto/aa_direct_output.cpp
    src/android_auto/aa_protocol.cpp
    src/android_auto/aa_tls.cpp
    src/android_auto/mbedtls_vita_platform.c

    src/audio/audio_output.cpp
    src/audio/audio_sink.cpp
    src/audio/microphone_input.cpp

    src/inputs/vita_controller_input.cpp
    src/inputs/vita_input.cpp
    src/inputs/vita_touch_input.cpp

    src/logger/vita_log.cpp

    src/transport/usb_transport.cpp
    kernel/client/loader.c

    src/video/gxm_scaler.cpp
    src/video/h264_annexb.cpp
    src/video/video_pipeline.cpp
    src/video/video_presenter.cpp
    src/video/status_screen.cpp
    src/video/vita_h264_decoder.cpp
)
# Failure logging is always available. Optional diagnostics are build choices.
target_compile_definitions(vita_auto PRIVATE
    VITA_AUTO_VERBOSE_LOGGING=$<BOOL:${VITA_AUTO_VERBOSE_LOGGING}>
    VITA_AUTO_PERFORMANCE=$<BOOL:${VITA_AUTO_PERFORMANCE}>
)
if(VITA_AUTO_PERFORMANCE)
    target_sources(vita_auto PRIVATE src/logger/performance.cpp)
endif()

target_include_directories(vita_auto PRIVATE
    src
    kernel/include
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvita2d/libvita2d/include"
)

target_link_directories(vita_auto PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/kernel"
)

target_link_options(vita_auto PRIVATE
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/cmake/vita_module_info.ld"
)
set_property(TARGET vita_auto APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vita_module_info.ld"
)

target_link_libraries(vita_auto PRIVATE
    vita_auto_usb_c_stub
    mbedtls
    mbedx509
    mbedcrypto
    SceLibKernel_stub
    SceProcessmgr_stub
    SceIofilemgr_stub
    SceSysmem_stub
    SceSysmodule_stub
    SceVideodec_stub
    SceAudio_stub
    SceAudioIn_stub
    SceTouch_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    ScePvf_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceShellSvc_stub
    m
)

add_dependencies(vita_auto
    vita_auto_usb_client_stubs
)

# libvita2d is used for the Vita display path.
set(VITA2D_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvita2d/libvita2d")
target_sources(vita_auto PRIVATE
    "${VITA2D_DIR}/source/bin_packing_2d.c"
    "${VITA2D_DIR}/source/int_htab.c"
    "${VITA2D_DIR}/source/texture_atlas.c"
    "${VITA2D_DIR}/source/vita2d.c"
    "${VITA2D_DIR}/source/vita2d_texture.c"
    "${VITA2D_DIR}/source/vita2d_draw.c"
    "${VITA2D_DIR}/source/vita2d_pvf.c"
    "${VITA2D_DIR}/source/utils.c"
    "${VITA2D_DIR}/shader/compiled/clear_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/clear_v_gxp.o"
    "${VITA2D_DIR}/shader/compiled/color_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/color_v_gxp.o"
    "${VITA2D_DIR}/shader/compiled/texture_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/texture_tint_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/texture_v_gxp.o"
)

vita_create_self(
    vita_auto.self
    vita_auto
)

vita_create_vpk(
    vita_auto.vpk
    ${VITA_TITLEID}
    vita_auto.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
    FILE assets/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_BINARY_DIR}/kernel/vita_auto_usb_client.suprx
         vita_auto_usb_client.suprx
)

# Keep intermediate files under build/, but mirror the installables
# into /dist
set(VITA_AUTO_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_custom_target(vita_auto_dist ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VITA_AUTO_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/vita_auto.vpk"
            "${VITA_AUTO_DIST_DIR}/vita_auto.vpk"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/kernel/vita_auto_usb.skprx"
            "${VITA_AUTO_DIST_DIR}/vita_auto_usb.skprx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/kernel/vita_auto_usb_client.suprx"
            "${VITA_AUTO_DIST_DIR}/vita_auto_usb_client.suprx"
    COMMENT "Copying Vita Auto installable files to dist/"
)


foreach(_vita_auto_dep
        vita_auto.vpk-vpk
        vita_auto.vpk
        vita_auto_usb.skprx-self
        vita_auto_usb.skprx
        vita_auto_usb_client.suprx-self
        vita_auto_usb_client.suprx)
    if(TARGET ${_vita_auto_dep})
        add_dependencies(vita_auto_dist ${_vita_auto_dep})
    endif()
endforeach()
