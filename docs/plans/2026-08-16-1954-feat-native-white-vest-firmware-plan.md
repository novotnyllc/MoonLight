# Native White Vest Firmware Plan

## Goal

Ship one identifiable Dig-Next-2 firmware build that contains the ordered 95-pixel vest geometry natively and fixes the reconnect loop without altering stored Wi-Fi or activating LED output during the flash.

## Settled decisions

- Target only PlatformIO environment `esp32-d0-pico2` for QuinLED Dig-Next-2 v1r3b.
- Compile the vest geometry into C++; do not use Live Script.
- Register `White Vest 95` as a selectable 3D layout, but do not change the empty default or persisted node selection.
- Clear the Wi-Fi connection attempt only on `ARDUINO_EVENT_WIFI_STA_GOT_IP`, not association.
- Identify this build as `1.0.0-whitevest.2` dated `20260816`; publish tag `v1.0.0-whitevest.2`.
- Read the OTA selection first, then flash only the selected app partition (`app0` at `0x10000` or `app1` at `0x340000`) without erase, preserving filesystem and NVS settings.

## Requirements

- R1: Native layout contains exactly the 95 `L_Vest.sc` coordinates in physical/index order and one `nextPin()` boundary.
- R2: Firmware builds for `esp32-d0-pico2` and leaves the vest layout inert until explicitly selected.
- R3: DHCP must complete before the reconnect timer considers a station connection successful.
- R4: `/rest/systemStatus` and artifact naming must distinguish the custom build.
- R5: Identify and capture the complete active app partition with a checksum; flash and rollback must use that app slot without erase.
- R6: Publish reviewed, secret-free source and tag the exact custom version.

## Work units

### U1 — Native vest layout

Add `src/MoonLight/Nodes/Layouts/L_WhiteVest95.h`, include it from `src/MoonBase/Nodes.h`, and register it in both layout enumeration and allocation paths in `src/MoonLight/Modules/ModuleDrivers.h`. Keep the raw atlas tuples visibly comparable with the canonical map, but scale them to a collision-free `6×6×20` virtual space before calling `addLight`; the raw `0..255` extent allocates about 16.45 million dense cells and trips the task watchdog. Resolve the persisted `/L_WhiteVest95.sc` name to this native layout before the generic Live Script fallback.

Evidence: an automated comparison against `vest/firmware/moonlight/L_Vest.sc` in the handoff repo proves all 95 coordinates in exact order, 95 unique scaled coordinates, 14 unique around-body columns, a 720-cell dense extent, and one pin boundary; the target build succeeds. The unrelated 14×8 Live Script geometry is explicitly excluded.

### U2 — Connectivity and identity

Change the existing station event handler in `lib/framework/WiFiSettingsService.cpp` from `STA_CONNECTED` to `STA_GOT_IP`, update its comment, and set `APP_VERSION`/`APP_DATE` in `platformio.ini`.

Evidence: focused diff review and successful target compilation; post-flash serial output shows association followed by DHCP without the prior repeated reconnect.

### U3 — Flash, verify, publish

Read OTA selection and checksum the complete active rollback partition and custom app binary. With the user-confirmed absence of 12 V input and LED terminal wiring as the output-off safety condition, perform an app-only serial flash to the selected app slot. Then verify boot, network reachability, custom system status, and native layout registration without changing the persisted driver selection. Selecting the 95-light layout and validating physical pixels is a separate post-wiring gate. Stop on panic/reset loop or lost connectivity. Push a reviewed branch, merge it into the public fork, tag the release, build the tag, and ensure the installed bytes/version match the published build.

## Definition of done

- Narrow coordinate check and `pio run -e esp32-d0-pico2` exit zero without masked failures.
- Focused diff and secret scan are clean; an independent review has no blocking findings.
- Active slot and prior/custom app images have recorded SHA-256 checksums.
- Device boots the custom version, allocates 720 virtual cells/2,880 bytes for exactly 95 LEDs, obtains HouseNet IoT DHCP, remains reachable, reports the expected target/version, and keeps output off. Authentication failure of the preserved Wi-Fi profile is a separate unresolved configuration gate, not firmware stability proof.
- Source is merged to `novotnyllc/MoonLight`, tag `v1.0.0-whitevest.2` is published, and the final flashed image is the tagged artifact.
