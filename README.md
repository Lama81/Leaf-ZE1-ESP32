# Leaf-ZE1-ESP32

*[Version francaise](README.fr.md)*

DIY firmware to remotely control a Nissan Leaf ZE1 (2019+): lock/unlock the doors, precondition the climate control, and monitor battery/vehicle status live, either locally over WiFi (next to the car) or from anywhere via a cellular LTE module (Particle Boron) and a web page.

Personal project, not affiliated with Nissan. The CAN frames (wake-up, lock/unlock, battery decoding) come from reverse engineering (see [Sources](#sources)); some are confirmed on the vehicle, others are documented best-effort guesses in the code.

## Features

- Door lock/unlock (reliable, primary goal)
- Climate preconditioning (adjustable temperature), with guards against the BCM's spontaneous restart
- Battery monitoring (SOC/SOH), climate, vehicle state: live local web dashboard (SSE)
- Remote control over LTE (Boron + Particle Cloud), no app to install, web page installable as a PWA or a native APK (see [App install](#app-install-pwa-or-apk))
- ESP32 deep sleep (wakes on remote command or every 6h) to limit drain on the vehicle's 12V battery while parked

## Architecture

```
Phone (webapp) --HTTPS--> Particle Cloud --LTE--> Boron --UART--> ESP32 --CAN--> Car
                                                                     |
Phone (local WiFi, leafcan) -------------------------------------- local AP
```

- **ESP32**: a single physical TWAI (CAN) controller, multiplexed between two logical buses on the car: EV-CAN (continuous passive listening: battery, climate, vehicle state) and CAR-CAN (transient transmission: wake-up, lock/unlock, climate). Also serves a local web dashboard (WiFi AP `leafcan`) and OTA updates.
- **Boron** (Particle, LTE): relay between the Particle cloud and the ESP32 over UART. Each cloud command (`lock`, `unlock`, `heat`, `status`, `wifi`) is sent as plain text and waits for a response before answering the cloud: no polling, no cellular data used outside an explicit action.
- **webapp**: static page (HTML/CSS/JS, no framework) that talks directly to the Particle cloud API from the browser. Installable as an app (PWA) on a phone.

Technical architecture details (TWAI mutex, climate guards, CAN frame decoding, concurrency): see [`CLAUDE.md`](CLAUDE.md).

## Repo structure

```
firmware/leaf-fw/   ESP32 - ESP-IDF firmware (single file main/main.c)
boron/               Particle Boron - LTE relay (src/boron.cpp)
webapp/              Static web page - dashboard + remote control
CLAUDE.md            Detailed technical reference (architecture, guards, CAN decoding)
```

## Hardware

- **ESP32 dev board** (plain `esp32` target, see `sdkconfig`; not an S2/S3/C3 variant).
- **2x CAN transceiver** (one per bus, 3.3V logic level compatible with the ESP32's TWAI TX/RX pins).
- **Particle Boron** (LTE cellular module, for remote control and telemetry beyond local WiFi range).
- **12V-to-logic power regulation**, tapped from the vehicle's 12V system, to power the ESP32 and Boron.
- **A physical connection point on the vehicle's CAN buses** (connector/location is model and generation specific; not documented here).

Exact part numbers, enclosure, and physical wiring/soldering details aren't finalized in this repo yet; see [`CARNET_DE_BORD.md`](CARNET_DE_BORD.md) (French) for what has been logged so far.

## Wiring / GPIO pins (ESP32)

| Function | ESP32 GPIO | Boron side | Notes |
|---|---|---|---|
| EV-CAN TX | GPIO32 | n/a | Listen-only mode, always active |
| EV-CAN RX | GPIO33 | n/a | |
| CAR-CAN TX | GPIO26 | n/a | Normal mode, transient transmission only |
| CAR-CAN RX | GPIO14 | n/a | |
| UART TX → | GPIO19 | RX (D10) | ESP32 ↔ Boron link, 9600 baud |
| UART RX ← | GPIO21 | TX (D9) | |
| Wake (EXT0) ← | GPIO34 | D8 | High level = deep sleep wakeup. **Requires an external pull-down resistor** (GPIO34-39 have no internal pull on the original ESP32) |
| Common GND | n/a | GND | Required between ESP32 and Boron |

A CAN transceiver (2x, one per bus) is required between the ESP32's TWAI GPIOs and the vehicle's real CAN buses; not documented here, adapt to the model used.

## Build & flash

### ESP32 (firmware/leaf-fw)

Prerequisites: ESP-IDF toolchain installed (target `esp32`), `idf.py` on the shell PATH.

```
cd firmware/leaf-fw
idf.py build
idf.py -p <PORT> flash monitor      # first flash, over USB
```

After the first USB flash, subsequent updates can be done over OTA: connect to the `leafcan` WiFi and open `http://192.168.4.1/update` to upload the `.bin` (`build/leaf-fw.bin`).

### Boron (boron/)

Prerequisites: [Particle CLI](https://docs.particle.io/getting-started/developer-tools/cli/) installed, logged into your account, device claimed.

```
cd boron
particle cloud flash <device_id_or_name>     # compiles in the Particle cloud + OTA flash (LTE)
```

Or locally over USB (uses no cellular data):
```
particle compile boron src --target 6.4.1 --saveTo target/6.4.1/boron/boron.bin
particle flash --usb target/6.4.1/boron/boron.bin
```

### webapp (webapp/)

Static page, no build step. Host the folder as-is on any HTTPS service (Cloudflare Pages/Workers, GitHub Pages, Netlify...). HTTPS is required for PWA installation (home screen icon, fullscreen mode).

## App install (PWA or APK)

The page can be installed two ways on Android:

- **PWA**: in Chrome, menu (three dots) then "Install app". Requires `manifest.json` + a service worker (`sw.js`, already included) reachable without authentication. If the page sits behind a login wall (e.g. Cloudflare Access), carve out an exception for these specific files, otherwise Chrome can't build the installable app.
- **Native APK (TWA)**: for true fullscreen without depending on Chrome's PWA behavior, the page can be packaged into a `.apk` with [Bubblewrap](https://github.com/GoogleChromeLabs/bubblewrap) (Google's official tool). Requires Node.js, a JDK 17, and the Android SDK command-line tools. "Trusted" fullscreen (no browser bar) also requires `/.well-known/assetlinks.json` (SHA-256 fingerprint of the app's signing certificate) to be reachable without authentication, same note as for the manifest.

See [`CARNET_DE_BORD.md`](CARNET_DE_BORD.md) (French) for the detailed APK build and access config used on the reference deployment.

## Configuration before flashing

- **Local WiFi**: copy `firmware/leaf-fw/main/wifi_secrets.h.example` to `wifi_secrets.h` (gitignored) and fill in your own `WIFI_SSID`/`WIFI_PASS` before flashing.
- **Native APK (optional)**: if you build your own APK (see [App install](#app-install-pwa-or-apk)), copy `webapp/.well-known/assetlinks.json.example` to `assetlinks.json` (gitignored) and fill in your own `package_name`/`sha256_cert_fingerprints`.
- **Device ID / Particle token**: entered in the web page, Configuration section (bottom). Stored only in `localStorage` on each device, never in the source code.
  - Create a token that never expires: `particle token create --never-expire` (default tokens expire after 90 days).

## Security

- If the webapp is hosted anywhere beyond strictly private use, adding an authentication layer in front (e.g. Cloudflare Access) is recommended. The page itself has no internal protection beyond the Particle token.

## Deep sleep

The ESP32 can enter deep sleep after a period of inactivity (no HTTP request or UART command) to limit consumption while the car is parked for a long time. Two wakeup sources: remote command via the Boron (GPIO34) or a periodic timer (6h, silently refreshes SOC/SOH without ever turning on WiFi). The local web dashboard is unreachable while the ESP32 sleeps; the intended main usage is through the Boron/webapp, not direct access.

## Sources

CAN frames and command sequences based on [OVMS `vehicle_nissanleaf.cpp`](https://github.com/openvehicles/Open-Vehicle-Monitoring-System-3) and the [`dalathegreat/leaf_can_bus_messages`](https://github.com/dalathegreat/leaf_can_bus_messages) DBC.
