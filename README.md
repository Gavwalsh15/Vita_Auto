# Vita Auto

Native Android Auto head unit for a PlayStation Vita.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/H7G22878U4)

Vita Auto is experimental Vita homebrew. It uses a kernel USB plugin to put the
Vita in USB host/accessory mode, then runs a native Android Auto projection
session with video, audio, microphone, and input support.

## Quick Start

Read [docs/setup.md](docs/setup.md) before installing. The Vita needs the right
USB OTG/power arrangement: the phone must see a stable 5 V USB host, and the
Vita alone does not provide a normal powered USB-A host port.

Release zips contain:

| File                         | Install manually? | Purpose                                                                          |
| ---------------------------- | ----------------- | -------------------------------------------------------------------------------- |
| `vita_auto.vpk`              | Yes               | Installs the Vita app                                                            |
| `vita_auto_usb.skprx`        | Yes               | Kernel USB plugin loaded by taiHEN                                               |
| `vita_auto_usb_client.suprx` | No                | User-mode bridge: the VPK also bundles this as `app0:vita_auto_usb_client.suprx` |

Short version:

1. Install `vita_auto.vpk`.
2. Copy `vita_auto_usb.skprx` to a taiHEN plugin location.
3. Add it under `*KERNEL` in `ur0:tai/config.txt`.
4. Reboot the Vita.
5. Open Vita Auto and connect the phone through a powered OTG setup.

`vita_auto_usb_client.suprx` does not need to be copied to `ur0:tai` or
`ux0:tai`: the app loads it from inside the VPK install.

## Documentation

- [Roadmap](docs/roadmap.md): current priorities and features under consideration.
- [Setup and install](docs/setup.md): hardware, OTG/power notes, taiHEN config,
  release files, controls, logs, and bug report details.
- [Wireless Android Auto (beta)](docs/wireless.md): Wi-Fi setup.
- [Architecture](docs/architecture.md): source layout, USB bridge, app session,
  audio, input, video, logging, CI, and release flow.
- [Core tasks and low-level architecture](docs/core-tasks.md): runtime task
  ownership, USB data flow, synchronization, and shutdown invariants.

## Build

Clone with dependencies included:

```bash
git clone --recurse-submodules https://github.com/Gavwalsh15/Vita_Auto.git
```

For an existing checkout:

```bash
git submodule update --init --recursive
```

The VitaSDK environment must provide `VITASDK` before configuring CMake. Git
must also be installed: CMake applies
`patches/mbedtls-x509-legacy-timezone.patch` before compiling Mbed TLS.

Installable files are written to
`dist/`.

## License

Copyright (C) 2026 Gavin Walsh.

Vita Auto project code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by the Free
Software Foundation, version 3. See [LICENSE](LICENSE).

Third-party dependencies under `third_party/` retain their own licenses. Release
artifacts are built from the tagged source tree, including the pinned submodules
and the checked-in Mbed TLS patch.
