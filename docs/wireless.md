# Wireless Android Auto (beta)

Wireless mode connects to the phone over Wi-Fi instead of USB. The Vita dials
the Headunit Server that Android Auto exposes in its developer settings, so the
phone needs Android Auto's developer mode turned on.

Status: In devlopment.

## Phone setup

1. Open Android Auto settings and tap the version header 10 times to enable
   developer mode.
2. In the developer menu choose **Start head unit server**. You may need to do
   this again after the phone restarts.
3. Turn on the phone's Wi-Fi hotspot.

## Vita setup

1. Join the phone's hotspot in the Vita's Settings app.
2. Open Vita Auto. In the default **Auto** mode it listens for a USB cable and
   the Headunit Server at the same time and uses whichever shows up first, so
   there is nothing to switch.

The connection mode is shown on the waiting screen. Press **Square** to cycle
Auto, USB only and Wi-Fi only.

If `vita_auto_usb.skprx` is not installed, the app has no USB to offer, so it
always runs in Wi-Fi only mode.

The Vita connects to the phone on TCP port 5277. If the
phone is not the default gateway, set `host=<phone IP>` in
`ux0:data/vita_auto/vita_auto_settings.txt` (see [setup.md](setup.md)).

## How it works

`src/transport/tcp_transport.cpp` implements the same `Transport` interface as
the USB transport. Unlike USB, no kernel plugin negotiates the Android Auto
version, so `DirectSession::start()` sends VERSION_REQUEST itself and begins TLS
once the phone accepts it. Everything above the transport is unchanged.

`src/transport/auto_transport.cpp` polls every enabled transport in one loop. The
first to connect wins and the others are closed immediately. If one side cannot start
(for example the USB plugin is not installed), Auto carries on with the other.
A USB handshake failure is reported straight away rather than waiting on Wi-Fi.

## Known limits

- The Vita's Wi-Fi is 2.4 GHz only. Expect more latency and jitter than USB.
- Wireless without developer mode (the phone finding the Vita over Bluetooth)
  is not supported yet.
