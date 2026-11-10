# Wireless Android Auto (beta)

This page is about Android Auto. Apple CarPlay ([carplay.md](carplay.md)) is wireless
too and shares the Bluetooth stack and the Vita's access point described here, but it
is a separate mode (`carplay=on`) and does not use the Headunit Server or the
bootstrap below.

Wireless mode connects over Wi-Fi instead of USB. Auto mode (the default) waits for
USB and both wireless paths at once and uses whichever connects first:

- **Headunit Server** (works on a test phone): the Vita dials the server in Android
  Auto's developer settings. No Bluetooth involved.
- **Bluetooth bootstrap** (experimental): a paired phone finds the Vita itself.

Expect rough edges. The Vita's Wi-Fi is 2.4 GHz only, so expect more jitter than USB.

## Headunit Server

Phone: tap the Android Auto version header 10 times, choose **Start head unit
server** in the developer menu, and turn on the Wi-Fi hotspot (2.4 GHz).
Vita: join the hotspot in Settings, then open Vita Auto. The Vita connects to the
gateway on TCP port 5277; if the phone is not the gateway, set `host=<phone IP>` in
the settings file ([setup.md](setup.md)). Some hotspots isolate clients, or use
10.x.x.x ranges where the Headunit Server reportedly fails.

**Square** cycles Auto / USB only / Wi-Fi only / CarPlay and applies immediately. Without
`vita_auto_usb.skprx` the app runs Wi-Fi only, and only the Headunit Server path
exists (the Bluetooth bootstrap lives in the plugin).

`src/transport/tcp_transport.cpp` implements the same `Transport` interface as USB.
`src/transport/auto_transport.cpp` polls all enabled transports; the first to connect
wins and the others close. A USB handshake failure is reported immediately.

## Bluetooth bootstrap (experimental)

Enabled when the app opens, disabled when it closes (nothing touches Bluetooth at
boot). While enabled the plugin swaps the Imaging Responder SDP record in SceBt for
an Android Auto wireless record and listens; the Vita shows up in the phone's
Bluetooth list as **Vita Auto**.

1. Put the Vita and phone on the **same Wi-Fi network**, a router or separate
   hotspot, not the phone's own hotspot (a phone cannot join its own hotspot: it
   reports status -11, or -10 with Wi-Fi off). The Vita is 2.4 GHz only.
2. Pair the phone once from the Vita's Settings > Devices > Bluetooth Devices; Vita
   Auto does not answer pairing requests. An `id=0x04 (pairing code)` line in the
   `bt-event:` log (diagnostic build) means the phone is asking to pair again.
3. Put the network password in the settings file (the Vita cannot read it back):
   `wifi_password=your-wifi-password`.
4. Open Vita Auto. When the paired phone connects, the Vita sends it its address
   and network details and listens on TCP port 5288; the phone opens the
   connection and the session starts. `bluetooth=off` skips the bootstrap.

The app sets the system device name to `bluetooth_name` (empty keeps yours) while
enabled and restores it on exit, keeping the original in `original_device_name` so
a crash is repaired on the next launch. If the Vita stays "Vita Auto", reset it in
Settings > System > Device Name.

In a car there is no router: use the phone's hotspot with the Headunit Server
(developer menu each trip), a small travel router the phone can join (2.4 GHz-only
name, or the phone may roam to 5 GHz and Android Auto drops), or the Vita's own
access point below.

The `bt-bootstrap:` log lines record every message; ids, fields and order were
checked against WirelessAndroidAutoDongle, but the port (5288) and BSSID text format
are unconfirmed against a real phone.

## The Vita's own access point (experimental)

For a car with no router and no developer menu: with `wifi_ap=on`, when the app
opens and the Vita is **not** on any Wi-Fi network, it raises an access point and
the bootstrap tells the phone to join that. If the Vita is already on a network,
that network is used.

```
wifi_ap=on
wifi_ap_ssid=Vita Auto
wifi_ap_password=vitaauto    8-63 characters, WPA2 (empty = open network)
wifi_ap_channel=6            1-11
```

**Status:** works on 3.65: the AP comes up, a phone associates, gets a DHCP lease
and joins. Sending needed two fixes in SceWlanBt (NID 0x94fa8830): the output
callback (0x7348) frees every frame unless a station is connected, so the plugin
wakes the transmit worker itself; and the transmit worker (0x235c) zeroes each
TxPD (bss_type station), so the SDIO write hooks set bss_type 1 (uAP) on data frames.
`wifi-ap: tx kicks N frames N stamped N` shows how far frames get.

Requirements and hazards:

- Unsafe app (the DHCP server needs port 67): **Enable Unsafe Homebrew** on.
- Firmware 3.65 only; the plugin refuses otherwise (`wifi-ap: failed: not the
  firmware...`).
- Wi-Fi **on** but not connected. The plugin waits up to 60 s for a driver command
  it can see (`Wi-Fi driver idle` means it never did).
- **Do not leave the app with the AP up**: 3.65 kernel-panics when any app launches
  or exits while it is up. The app drops it when a session ends or the app closes,
  and the plugin drops it within 1 s if the app stops responding (suspended or
  crashed). Closing from LiveArea inside that second can still crash the Vita.
- Joining a Wi-Fi network while the AP is up is untested and may crash.

`kernel/vita_auto_ap.inc` sends the Marvell commands (UAP_SYS_CONFIG, UAP_BSS_START)
through SceWlanBt's command path, after giving a fresh driver 10 s to settle. The
BSSID is the Wi-Fi MAC with the locally administered bit set; the address is
192.168.77.1, set on `wlan0` by the bridge, and `src/transport/wifi_ap.cpp` is a
small DHCP server handing out .10 to .13. `wifi-ap:` log lines show association and
DHCP counters every 10 s.
