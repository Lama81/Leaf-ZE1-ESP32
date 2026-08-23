# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an ESP32 that bridges to a Nissan Leaf ZE1's two CAN buses (EV-CAN and CAR-CAN) over WiFi. It lets a phone/browser lock/unlock the car, control climate preconditioning, and monitor battery/vehicle telemetry live, entirely over a self-hosted WiFi access point (no cloud, no app). Primary goal right now: reliable LOCK/UNLOCK over CAR-CAN. Climate/HVAC control works but is considered secondary.

The entire firmware is one file: `firmware/leaf-fw/main/main.c` (~870 lines). There is only one ESP-IDF component (`main`); do not assume a multi-component structure.

## Build / flash

Requires the ESP-IDF toolchain with `IDF_PATH` set up in the shell (`idf.py` must be on PATH; this repo does not vendor ESP-IDF).

```
cd firmware/leaf-fw
idf.py build
idf.py -p <PORT> flash monitor      # first flash over USB
```

Target chip is plain `esp32` (see `sdkconfig`'s `CONFIG_IDF_TARGET`).

After the first USB flash, updates can be pushed wirelessly: connect to the `leafcan` AP and open `http://192.168.4.1/update` to upload the `.bin` from `build/leaf-fw.bin`. The device reboots into the new image automatically (see `ota_0`/`ota_1` partitions in `partitions.csv`).

There is no test suite, linter, or CI in this repo. Validation happens by compiling with `idf.py build` and testing on the actual vehicle/hardware.

## Working method (from PROJECT_CONTEXT.md, still the expected workflow)

- Never rewrite the whole firmware in one go. Make one targeted change at a time.
- Cycle: modify → `idf.py build` → test on the real vehicle → commit.
- CAN payloads in this file (wakeup frames, lock/unlock bytes, decode bit offsets) come from reverse-engineering (OVMS `vehicle_nissanleaf.cpp`, the `dalathegreat/leaf_can_bus_messages` DBC) and are *not* all vehicle-confirmed. Treat values noted as unconfirmed/"a ajuster" in comments as best-guess, not ground truth. Do not silently change these encodings without a comment explaining why.
- Comments and prior project notes are in French; matching that language in comments for CAN/vehicle-domain code is consistent with the existing style, but not mandatory.

## Architecture

### Dual CAN bus, one TWAI peripheral

The ESP32 has a single TWAI (CAN) controller but the car exposes two logically separate buses on different pins, and the firmware time-multiplexes the single controller between them:

- **EV-CAN** (GPIO32 TX / GPIO33 RX): listen-only, always-on. Used for passive monitoring: battery SOC/SOH/voltage/current, climate status, vehicle on/off, charge status. This is what `can_task` runs continuously.
- **CAR-CAN** (GPIO26 TX / GPIO14 RX): normal mode, transmit. Used only transiently to send commands (wakeup, lock/unlock, climate on/off).

Switching buses means: `twai_stop()` + `twai_driver_uninstall()` on the current config, then `twai_driver_install()` + `twai_start()` with the other pin/mode config. This happens inside `do_lock_sequence()`, `do_heat_sequence()`, `do_heat_off_sequence()`: each of these takes `twai_mutex`, sets `transmitting = true` so `can_task`'s RX loop backs off, flips to CAR-CAN, sends a wakeup frame (`carcan_wakeup()`, ID `0x68C` then `0x56E`) plus the repeated command frames, then flips back to EV-CAN listen-only before releasing the mutex. Any new command sequence must follow this same take-mutex / stop-uninstall-reinstall-start / restore-EV-CAN / release-mutex pattern; skipping a step leaves the bus in the wrong mode or leaves EV-CAN monitoring dead.

Command frames are sent repeatedly (`CARCAN_REPEAT_COUNT_LOCK` / `CARCAN_REPEAT_COUNT_HEAT`, ~100ms apart) rather than once, since the exact repeat count needed by the BCM isn't confirmed.

### Climate auto-restart guards

The BCM can leave its climate latch open and have climate spontaneously restart ~30 min after the car is shut off. Two independent guards defend against this, both centered on `g_climate_active`:

- **Guard 1 (event-driven, primary)**: `can_task`'s RX loop watches ID `0x11A` (`CarOnOffStatus`). It only fires `do_heat_off_sequence()` on an actual ON→OFF *transition* while climate is active; it tracks this via `g_veh_on_since_climate`, which must be observed `true` (vehicle seen ON) before an OFF is treated as a real transition. This deliberately avoids re-triggering OFF when the car is just parked with climate off already, e.g. during remote preconditioning.
- **Guard 2 (watchdog, secondary)**: `g_climate_watchdog` (esp_timer, `CLIMATE_WATCHDOG_TIMEOUT_US` = 20 min) is armed whenever a heat sequence starts and force-sends OFF if it fires while `g_climate_active` is still true, a backstop for when 0x11A is missed.

Both guards reset (`g_climate_active`, `g_veh_on_since_climate`, timer stop) at the end of `do_heat_off_sequence()`, which is the single place all climate-off paths (manual `/heat_off`, watchdog, guard 1) converge.

### CAN frame decoding

`decode_battery_frame()` decodes a handful of known EV-CAN IDs (`0x5BC` GIDS/SOH, `0x55B` SOC, `0x1DB` pack voltage/current, `0x54C` climate/ambient temp, `0x1D4` charge status, `0x11A` vehicle on/off) into human-readable suffixes appended to the raw hex log line. `0x1DC` (power limits) is recognized but intentionally left undecoded; bit layout unverified. When adding new IDs, follow the existing pattern: bit offsets/scale documented in a comment citing the source (DBC field name or start-bit/length), output appended via `snprintf` into the caller's buffer.

### Web server / UI

Single `httpd` instance (`start_webserver()`) serves everything, no external assets: HTML/CSS/JS are inline C string literals (`html_page`, `update_page`). Live monitor page uses Server-Sent Events (`/events`) fed by `can_queue` (a `QueueHandle_t` of `can_msg_t`), populated by `can_task`. In non-verbose mode only a filtered set of "interesting" CAN IDs get pushed to the queue (see the `interesting` bool in `can_task`); `/monitor?mode=all` toggles verbose passthrough of every frame.

Endpoints: `/` (monitor UI), `/events` (SSE stream), `/update` GET/POST (OTA page + upload), `/heat?temp=N`, `/heat_off`, `/monitor?mode=`, `/lock`, `/unlock`. `httpd_config_t.max_uri_handlers` is explicitly raised to 12 (default is 8); registering a 9th+ endpoint silently 404s past the default, so bump this further if adding more handlers than headroom allows.

WiFi AP credentials (`WIFI_SSID`/`WIFI_PASS`) live in `firmware/leaf-fw/main/wifi_secrets.h` (gitignored, see `wifi_secrets.h.example`). Pin assignments are `#define`s near the top of `main.c`.

### Concurrency model

Two tasks/contexts touch the TWAI peripheral: `can_task` (continuous RX) and the HTTP handler threads (via `do_lock_sequence`/`do_heat_sequence`/`do_heat_off_sequence`, invoked synchronously from `httpd` request handlers). `twai_mutex` serializes all access; `transmitting` is an additional fast-path flag so `can_task` skips its receive-and-bus-off-recovery work entirely while a command sequence owns the bus, rather than blocking on the mutex mid-transmit sequence.
