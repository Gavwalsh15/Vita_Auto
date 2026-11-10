# Vita Auto

A head unit for a PlayStation Vita: Android Auto (USB or wireless) and
wireless Apple CarPlay.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/H7G22878U4)

Vita Auto is experimental Vita homebrew. A kernel plugin puts the Vita in USB
host/accessory mode and a native Android Auto session runs with video, audio,
microphone and input. Android Auto also runs wirelessly (Wi-Fi, Bluetooth). Apple
CarPlay is wireless only, needs an accessory identity you supply, and is switched on
with `carplay=on` in the settings file; the app runs one of the two at a time.

## Quick start

The Vita needs a powered OTG/Y-cable setup so the phone sees a stable 5 V host (see
[docs/setup.md](docs/setup.md)).

1. Install `vita_auto.vpk` (turn on Enable Unsafe Homebrew first).
2. Copy `vita_auto_usb.skprx` to `ur0:tai/` and add it under `*KERNEL` in
   `ur0:tai/config.txt`.
3. Reboot, open Vita Auto and connect the phone (for CarPlay, see
   [docs/carplay.md](docs/carplay.md) first).

`vita_auto_usb_client.suprx` is loaded from inside the VPK; it needs no copying.

## Documentation

- [Setup](docs/setup.md): hardware, install, Android Auto or CarPlay, controls, settings, logs, troubleshooting.
- [Wireless Android Auto](docs/wireless.md): Headunit Server, Bluetooth bootstrap, the Vita's own access point.
- [Apple CarPlay](docs/carplay.md): pipeline, what you must supply, status.
- [Architecture](docs/architecture.md): source layout, what the two protocols share, tasks, data flow, shutdown rules, CI.

## Roadmap

Stablity Updates
[Open a feature request](https://github.com/Gavwalsh15/Vita_Auto/issues/new/choose)
or add your use case to an existing one.

## Build

```bash
git clone --recurse-submodules https://github.com/Gavwalsh15/Vita_Auto.git
git submodule update --init --recursive   # for an existing checkout
```

`VITASDK` must be set before configuring CMake, and Git must be installed (CMake
applies `patches/mbedtls-x509-legacy-timezone.patch` to Mbed TLS). Installable
files are written to `dist/`.

## License

Copyright (C) 2026 Gavin Walsh.

Vita Auto project code is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free Software
Foundation, version 3. See [LICENSE](LICENSE).

Third-party dependencies under `third_party/` retain their own licenses. Release
artifacts are built from the tagged source tree, including the pinned submodules and
the checked-in Mbed TLS patch.
