# Apple CarPlay (experimental)

Wireless CarPlay only (no USB), enabled with `carplay=on` in the settings file. The
app runs one protocol at a time, so while it is on Android Auto is not started (an
Android phone cannot connect); press **Square** on the connection screen (or set it back to
`off`) to use Android Auto ([setup.md](setup.md)). Device-tested: video, touch, the iAP2 tunnel,
pairing and a clean exit work. **Audio does not:** the phone opens no audio stream
(see [Known limits](#known-limits)).

## Pipeline

1. **Bluetooth iAP2.** The kernel module dials the paired iPhone, opens its iAP2
   RFCOMM service and passes the bytes up (`kernel/vita_auto_bt_client.inc`).
2. **Identification and MFi auth.** The phone sends `0x1D00`; the Vita identifies as
   a wireless CarPlay accessory and runs the MFi exchange (`0xAA00`-`0xAA05`) with
   your identity.
3. **Wi-Fi handoff.** The phone asks for the Wi-Fi configuration (`0x5702`) and is
   told which network to join (the Vita's access point, or the router it is on)
   and where the AirPlay receiver listens (`0x5703`, `0x4300`/`0x4301`).
4. **AirPlay.** The phone connects to port 7000. The Vita pairs (SRP, then
   Ed25519/X25519 verify), serves `/info` and `/auth-setup`, and sets up the screen,
   audio, timing, event and iAP2 data streams.
5. **Projection.** H.264 goes to the same decoder and presenter as Android Auto
   (`video::pipeline`), audio to the same audio output, touch and buttons back as HID reports on the event
   channel. Stream 130 carries the second iAP2 exchange.

## Where the code comes from

The protocol code is not copied in: it comes from the sibling `nbtplay` project
(GPL-3.0). CMake finds nbtplay through
`VITA_AUTO_CARPLAY_CORE` (default `../beamer-dreamer/nbtplay/src`); without it the
app still builds and `carplay` reports the core is absent.

| Piece | Where |
| --- | --- |
| iAP2 link, framing, auth; AirPlay pairing, RTSP, crypto, media | nbtplay (`iap2_*`, `auth`, `airplay`, `pairing`, `srp`, `rtsp`, `bplist`, `crypto`, `media`, `screen`, `audio`, `timing`, `events`) |
| Wireless identification, handoff, tunnel endpoint | `src/carplay/iap2_endpoint.cpp` |
| Identity from files | `src/carplay/mfi_authenticator.cpp` |
| AirPlay control (touch `/info`, PCM/AAC, iAP2 stream) | `src/carplay/airplay_control.cpp` |
| Sockets, tunnel, pairing state | `src/carplay/carplay_session.cpp` (+ `carplay_media.cpp` video/audio, `carplay_input.cpp` touch/buttons) |
| Bonjour announce + control lookup | `src/carplay/bonjour.cpp` |
| Orchestration (Bluetooth, network, session loop) | `src/carplay/carplay_bootstrap.cpp` |
| Bluetooth dial-out and RFCOMM | `kernel/vita_auto_bt_client.inc` |

## Your accessory identity

The iPhone checks an Apple MFi certificate in step 2. **This project ships no
identity, does not generate or extract one, and does not help obtain one.** It loads
files you supply from `ux0:data/vita_auto/carplay/`: `identity.pk8` (private key,
PKCS#8 DER, P-256) and `certificate.p7b` (its certificate, sent as stored).
Without both, the log says `no accessory identity` and the run ends
at the phone's first authentication request. AirPlay pairing state is kept in
`pairing.bin` beside them; delete it to forget every phone.

## Other prerequisites

- **Pair the iPhone first.** SceBt refuses unsupported device classes at pairing; use
  the "allow unsupported device" SceBt patch (VitaControl-style; not applied here).
- **A network.** With the Vita off Wi-Fi the soft AP (`wifi_ap=on`, see
  [wireless.md](wireless.md)) is used; on a router, the phone is told to join it and
  `wifi_password` must be set.

## Testing

Install `vita_auto.vpk`, `vita_auto_usb.skprx` and `vita_auto_usb_client.suprx`
together (reboot after a kernel change; the app reports `the kernel plugin is not the
build this app was made with` if they differ), connect the paired iPhone and send the
log. Use the diagnostic build for the raw dumps. In order: `identity loaded`, `access
point up` / `using Wi-Fi network`, `AirPlay receiver listening`, `stack up; iAP2
client installed`, `link steps 0x...: <new steps> (rfcomm channel N ...)` (the step
names are the `VITA_AUTO_IAP2_*` bits in `kernel/include/vita_auto_usb.h`; `pipe`
means bytes can flow, `DM (refused)` means the phone rejected the data channel),
`phone message 0x...`, `the phone accepted the authentication`,
`wireless handoff sent`, then `phone connected to the AirPlay control port`, `phone
paired`, `first video frame` and the ten-second `paired= frames= audio= hid=` summary.

## Known limits

- **Audio.** The phone hands the audio resource to the Vita (`modesChanged` resource
  2 -> entity 1) but opens no audio stream and plays on its own speaker. Ruled out:
  keeping the Bluetooth link up, the Bluetooth address (the real one is read from
  SceBt `state + 0x10`), an AAC-LC offer for stream 102 (decoded by SceAudiodec,
  `src/audio/aac_decoder.cpp`), and a hands-free link (came up, changed nothing,
  removed). Apple's WWDC17 session 717 says Bluetooth is only for discovery.
- **Current lead** (untested): what DiPlay and LIVI both do and this code did not.
  `src/carplay/bonjour.cpp` announces `_airplay._tcp` (TXT: deviceid, features, flags,
  model, srcvers, protovers, `pi`, `pk`) and looks up the phone's `_carplay-ctrl._tcp`,
  then sends `GET /ctrl-int/1/connect` with `AirPlay-Receiver-Device-ID` (seven
  tries, alternating decimal and hex ids). First run: the phone answered none of 20
  queries and asked for our record only 3 times, so the diagnostic build dumps every mDNS
  packet (`carplay-ctrl: mdns ...`) to show what the phone publishes. Session SETUP
  also returns a `keepAlivePort` (UDP 7012) when asked.
- After the phone's `disableBluetooth` and an authenticated tunnel (plus 2 s) the app
  drops the Bluetooth link (`vitaAutoBtDisconnect`).
- The router case sends channel 0 (unknown); Bluetooth reconnect after the phone
  leaves has only the plugin's behaviour.
- Opus is not decoded, the microphone is not sent, the voice button has no mapping.
  Apple Music may need DiPlay's buffered type-103 stream (`mainBuffered`); not done.
- Video uses the hardware decoder at what the phone sends (up to 1280x720, 30 fps);
  decoder timestamps are a known latency lead (the decoder holds 9-13 frames).

## Licence

nbtplay and this project are GPL-3.0; code taken from nbtplay keeps its notices.
