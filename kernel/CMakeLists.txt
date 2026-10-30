# Kernel plugin: owns USB hardware and exports user-callable syscalls.
# No C runtime is linked. keep memory operations compatible with -fno-builtin.
add_executable(vita_auto_usb_kernel
    vita_auto_usb_kernel.c
)

target_include_directories(vita_auto_usb_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(vita_auto_usb_kernel PRIVATE
    VITA_AUTO_VERBOSE_LOGGING=$<BOOL:${VITA_AUTO_VERBOSE_LOGGING}>
)

target_compile_options(vita_auto_usb_kernel PRIVATE
    -Wall
    -Wextra
    -O2
    -fno-builtin
    -nostdlib
)

target_link_options(vita_auto_usb_kernel PRIVATE
    -nostdlib
    -Wl,-q
)

target_link_libraries(vita_auto_usb_kernel PRIVATE
    SceUsbdForDriver_stub
    SceUsbServForDriver_stub
    SceCpuForDriver_stub
    SceDebugForDriver_stub
    SceSysmemForDriver_stub
    SceThreadmgrForDriver_stub
)

vita_create_self(
    vita_auto_usb.skprx
    vita_auto_usb_kernel
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
    UNSAFE
)

vita_create_stubs(
    vita_auto_usb_kernel_stubs
    vita_auto_usb_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
    KERNEL
)

# User-mode bridge: forwards the app API through the generated kernel stubs.
# client/loader.c is compiled into the app by the top-level CMakeLists.txt.
add_executable(vita_auto_usb_client
    client/vita_auto_usb_client.c
)

target_include_directories(vita_auto_usb_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/client
)

target_compile_options(vita_auto_usb_client PRIVATE
    -Wall
    -Wextra
    -O2
    -nostdlib
)

target_link_directories(vita_auto_usb_client PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_options(vita_auto_usb_client PRIVATE
    -nostdlib
)

target_link_libraries(vita_auto_usb_client PRIVATE
    vita_auto_usb_stub
    SceLibKernel_stub
)

add_dependencies(vita_auto_usb_client
    vita_auto_usb_kernel_stubs
)

vita_create_self(
    vita_auto_usb_client.suprx
    vita_auto_usb_client
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/client/exports.yml
    UNSAFE
)

vita_create_stubs(
    vita_auto_usb_client_stubs
    vita_auto_usb_client
    ${CMAKE_CURRENT_SOURCE_DIR}/client/exports.yml
)
