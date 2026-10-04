# Architecture

Vita Auto is split into a Vita app, a user-mode USB bridge, and a kernel USB
plugin. The app owns the Android Auto session and presentation. The kernel plugin
owns USB host mode, Android Open Accessory setup, bulk pipes, and transfer
cleanup.

For the runtime task map, USB data flow, synchronization rules, and shutdown
invariants, read [core-tasks.md](core-tasks.md).

## Source Layout

| Folder | Responsibility |
| --- | --- |
| `src/app` | Application lifecycle, connection screens, retry/exit flow |
| `src/android_auto` | Android Auto framing, service discovery, TLS, channel logic |
| `src/audio` | PCM playback routing and microphone capture |
| `src/inputs` | Vita controls, touch, input events, and event queue |
| `src/video` | H.264 parsing/decoding, frame ordering, and presentation |
| `src/logger` | File logging and optional performance reporting |
| `src/transport` | User-space USB transport used by the Android Auto session |
| `src/common` | Shared utilities |
| `kernel` | Kernel USB plugin, user-mode bridge, exports, and loader |

## Runtime Pieces

`vita_auto.vpk` installs the app. The VPK also contains
`vita_auto_usb_client.suprx`, which the app loads from
`app0:vita_auto_usb_client.suprx`.

`vita_auto_usb.skprx` is installed as a taiHEN kernel plugin. It owns USB host
mode and the Android Open Accessory negotiation.

`vita_auto_usb_client.suprx` is a small user-mode bridge. It imports the kernel
plugin syscalls and exports regular app-callable functions such as
`vitaAutoUsbStart`, `vitaAutoUsbStop`, `vitaAutoUsbWrite`, and
`vitaAutoUsbReadWaitTimed`.

## Startup Flow

1. `src/main.cpp` resets logging and calls the app runner.
2. `src/app/application.cpp` acquires the HOME/automatic USB connection locks
   through `src/app/usb_shell_lock.cpp`, then initializes input and display state.
3. The app starts the direct USB session in `src/app/direct_session.cpp`.
4. `src/transport/usb_transport.cpp` loads the client bridge from the VPK and
   calls into the kernel plugin.
5. The kernel plugin negotiates Android Open Accessory mode and the Android Auto
   version handshake.
6. `src/android_auto/aa_direct.cpp` starts TLS and Android Auto service setup.
7. Video, audio, microphone, and input begin once the phone opens the relevant
   Android Auto channels.

## Disconnect and Reconnect

Disconnect is handled as a normal session end where possible:

1. The app stops accepting input for the old session.
2. Producers are stopped before USB cleanup.
3. The USB session is closed and transfer callbacks drain.
4. Display resources are handed off before GXM/libvita2d resources are freed.
5. Recoverable disconnects schedule another attempt.

If USB or display cleanup cannot complete, the app asks for a Vita restart and
prevents another connection attempt. That is safer than reusing kernel/display
state whose ownership is unclear.

HOME stays locked until USB, input, video, and display cleanup all succeed.
Automatic retries retain the lock between attempts, so they cannot start USB
after the user has switched to LiveArea. **Start + Select** requests a manual
disconnect and disables automatic retries. The main loop waits for IO completion
before joining and cleaning up. It then releases the shell locks and shows
**Disconnected**. **Cross** acquires the locks again before starting a new session.

This follows VitaShell's use of `sceShellUtilLock` for the PS button and automatic
Content Manager USB connections. It does not interpret controller input as a
HOME event or run USB shutdown inside a system callback.

## Build Flags

The default build keeps non-critical logging and performance reporting off.

| Flag | Effect |
| --- | --- |
| `VITA_AUTO_VERBOSE_LOGGING` | Enables extra startup/runtime logs |
| `VITA_AUTO_PERFORMANCE` | Enables performance counters and reports |


Critical errors remain logged in every build.

## CI and Releases

GitHub Actions builds two flavors:

| Flavor | Logging | Performance |
| --- | --- | --- |
| quiet | OFF | OFF |
| diagnostic | ON | ON |

Tagged releases package both flavors and include checksums. Release files keep
their runtime names inside each zip so the VPK and loader paths continue to
match.

## Dependency Patch

Mbed TLS is kept as a submodule. During CMake configuration,
`cmake/ApplyMbedTlsPatch.cmake` applies
`patches/mbedtls-x509-legacy-timezone.patch` if needed. The patch lets the
Android Auto head-unit certificate load its legacy numeric timezone suffix while
leaving the original DER bytes untouched.
