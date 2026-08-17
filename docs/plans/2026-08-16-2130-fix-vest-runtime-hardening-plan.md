---
title: Dig-Next-2 Wearables Runtime Hardening - Plan
type: fix
date: 2026-08-16
artifact_contract: ce-unified-plan/v1
artifact_readiness: implementation-ready
product_contract_source: ce-plan-bootstrap
execution: code
---

# Dig-Next-2 Wearables Runtime Hardening - Plan

## Goal Capsule

Create one reusable Dig-Next-2 MoonLight runtime for the White Vest and coat. Keep garment geometry in separate native layouts while fixing large HTTP responses, local mDNS discovery, and the built-in PDM microphone once at the controller/runtime layer. Ship the runtime only after the tagged app binary is installed and sound-reactive plus physical LED behavior are observed on the White Vest validation device.

## Product Contract

### Summary

The shared Dig-Next-2 runtime must serve its web UI, advertise each device's configured hostname, acquire usable microphone samples, and drive garment-specific native layouts without reverting to WLED or Live Script.

### Problem Frame

The current `1.0.0-whitevest.2` build stays online and serves small REST responses, but responses around 2.1 KB and the root UI stall after headers. The firmware starts mDNS before DHCP completes, and the FastLED PDM input on the official Dig-Next-2 GPIO7/GPIO8 pins reports initialization failure. These failures block configuration and sound-reactive use even though the native layout is healthy.

### Requirements

**HTTP and discovery**

- R1. Root UI, file, JSON, and chunked responses must complete across the observed size boundary without blocking the HTTP task.
- R2. `white-vest-next.local` must resolve after DHCP and after a Wi-Fi reconnect.

**Audio and effects**

- R3. The built-in Dig-Next-2 PDM microphone must initialize on official GPIO7 data and GPIO8 clock assignments.
- R4. Live audio must produce changing volume and frequency-band data and visibly drive a sound-reactive effect.

**Release and physical proof**

- R5. The native White Vest layout must remain 95 RGB pixels on LED1/GPIO2 with a `6×6×20` mapping and 720 virtual cells.
- R6. Updates must remain app-only until physical validation succeeds, preserving NVS and LittleFS.
- R7. A plain 95-pixel solid-color test must pass before mapped or sound-reactive effects are accepted.
- R8. After all functional and Playa-hardening gates pass, export readable configuration and capture/checksum a private complete Dig-Next-2 flash containing the selected layout, curated presets/effects, audio settings, hostname/Wi-Fi, and security configuration for exact same-board restoration.
- R9. HTTP, mDNS, PDM audio, and packaging fixes must remain shared Dig-Next-2 behavior; garment layouts and hostnames must remain independent configuration.
- R10. One firmware image must enumerate every available native garment layout and persist the selected layout independently on each controller.

## Planning Contract

### Key Technical Decisions

- KTD1. **Characterize the response-size boundary before changing transport code.** One generated response seam must prove the failing threshold and the corrected single-buffer and chunked paths.
- KTD2. **Tie mDNS lifecycle to acquired network addresses.** Do not treat a pre-DHCP `MDNS.begin()` call as successful discovery.
- KTD3. **Keep the official microphone pins.** GPIO7/GPIO8 come from the QuinLED Dig-Next-2 pinout; correct the PDM driver or platform integration instead of guessing alternate wiring.
- KTD4. **Prove audio at the data and effect layers.** “PDM active” alone is insufficient; nonzero changing samples and a visible reactive effect are required.
- KTD5. **Preserve the proven layout and app-only recovery boundary.** No WLED, Live Script, filesystem erase, or full-flash write belongs in the fix cycle.
- KTD6. **Use a generic Dig-Next-2 build identity.** Publish the runtime as `1.0.1-dignext2.5`; do not encode `whitevest` or `coat` in shared firmware identity.
- KTD7. **Package layouts together.** Reuse the existing native layout registry so a vest or coat controller selects its own layout from the same firmware image.

### Scope Boundaries

- In scope: shared HTTP response handling, mDNS lifecycle, PDM capture, audio telemetry/effect proof, release, flash, physical solid-color test, and final restoration backup.
- Deferred: authoring the coat's native coordinates until a canonical coat pixel map exists. The shared firmware and registry support that layout without a separate build. Unrelated MoonLight UI changes and speculative mutex fixes also remain deferred unless runtime evidence reaches them.

## Implementation Units

### U1. Characterize and fix HTTP response completion

- **Goal:** Make large UI, JSON, and file responses complete without blocking the server.
- **Requirements:** R1, R6, R9
- **Dependencies:** None
- **Files:** `lib/PsychicHttp/src/PsychicResponse.cpp`, `lib/PsychicHttp/src/PsychicJson.cpp`, `lib/PsychicHttp/src/PsychicFileResponse.cpp`, `test/test_native/test_http_response.cpp`
- **Approach:** Instrument the existing response seam, identify where headers complete but body transmission blocks, and fix the shared owner rather than individual endpoints.
- **Execution note:** Start with a focused characterization check around the observed 2 KB boundary.
- **Test scenarios:** A response below the boundary completes; a response spanning multiple TCP segments completes; JSON and file responses report the exact body length; send failure terminates without a stuck request.
- **Verification:** `/`, `/rest/drivers`, and a static JSON file return complete bodies repeatedly while uptime continues.

### U2. Make mDNS follow the network lifecycle

- **Goal:** Advertise the configured hostname whenever Wi-Fi has a usable address.
- **Requirements:** R2, R9
- **Dependencies:** None
- **Files:** `lib/framework/ESP32SvelteKit.cpp`, `lib/framework/ESP32SvelteKit.h`, `lib/framework/WiFiStatus.cpp`, `test/test_native/test_network_lifecycle.cpp`
- **Approach:** Initialize mDNS while internal memory is available, re-enable and announce it after `GOT_IP`, and refresh the advertisement every 60 seconds so the hostname remains cached across the routed 120-second TTL.
- **Test scenarios:** Startup before DHCP does not claim discovery; first `GOT_IP` advertises the configured hostname; reconnect restores advertisement; repeated address events remain idempotent.
- **Verification:** `white-vest-next.local` resolves to `192.168.20.239` from the Mac after boot and reconnect.

### U3. Restore PDM audio and sound-reactive data

- **Goal:** Capture usable audio from the built-in microphone and feed MoonLight effects.
- **Requirements:** R3, R4, R9
- **Dependencies:** None
- **Files:** `src/MoonLight/Nodes/Drivers/D_FastLEDAudio.h`, `src/MoonBase/Modules/ModuleIO.h`, `firmware/esp32-d0.ini`, `test/test_native/test_audio_config.cpp`
- **Approach:** Trace the pinned FastLED PDM creation path against ESP32-PICO-V3-02 and Arduino-ESP32 3.3.7. Keep GPIO7/GPIO8 and correct only the confirmed configuration or platform incompatibility.
- **Test scenarios:** Official pins produce a valid PDM configuration; startup failure reports the underlying platform error; captured samples update volume and bands; silence stays near the noise floor; a sound event changes reactive effect input.
- **Verification:** Serial and driver status show active PDM, live telemetry changes with sound, and a sound-reactive effect changes visibly.

### U4. Build, review, publish, and install exact bytes

- **Goal:** Deliver a review-settled versioned app artifact and install that exact artifact.
- **Requirements:** R5, R6, R9, R10
- **Dependencies:** U1, U2, U3
- **Files:** `firmware/esp32-d0.ini`, `docs/plans/2026-08-16-2130-fix-vest-runtime-hardening-plan.md`
- **Approach:** Build only `esp32-d0-pico2`, publish generic version `1.0.1-dignext2.5`, flash the app partition, and compare release and installed evidence by SHA-256.
- **Test scenarios:** Clean target build; secret scan; independent review; release asset digest equals the flashed app artifact; boot preserves the native map and Wi-Fi profile.
- **Verification:** Merged PR, exact tag, release asset checksum, esptool write verification, stable boot, and passing HTTP/mDNS/audio checks.

### U5. Complete physical validation and restoration capture

- **Goal:** Prove the installed string and retain an exact post-validation recovery image.
- **Requirements:** R4, R7, R8
- **Dependencies:** U4
- **Files:** `docs/plans/2026-08-16-2130-fix-vest-runtime-hardening-plan.md`
- **Approach:** Ask for one physical action at a time. Validate low-brightness solid red, green, and blue across all 95 pixels before enabling the native map and sound-reactive effect. Curate and save the desired startup/effect presets, apply Playa security, export readable configuration, then read and checksum the complete flash only after every gate passes.
- **Test scenarios:** Every pixel responds in RGB order; no reset or visible corruption occurs; mapped effect orientation is plausible; sound changes the chosen reactive effect; complete flash read verifies by checksum.
- **Verification:** User-observed physical pass, stable controller status, and a recorded full-flash checksum and restore warning.

## Verification Contract

- Native tests cover the response boundary, network lifecycle, and audio configuration seams when those seams are host-testable.
- `pio run -e esp32-d0-pico2` must exit zero without masked failures.
- Device proof must include complete HTTP bodies, mDNS lookup, live microphone telemetry, serial stability, native-map invariants, and exact flashed-byte verification.
- Physical proof must use one action at a time and stop on zero lights, wrong voltage behavior, panic, reset loop, or connectivity loss.

## Definition of Done

- All R1-R8 requirements are satisfied with no abandoned experimental code in the diff.
- The public release contains only secret-free source, the app-only firmware, and checksums.
- The current controller resolves by hostname, serves the full UI, reports working audio, drives all 95 pixels, and runs a sound-reactive effect.
- A complete post-validation Dig-Next-2 flash image is stored privately with its checksum and is never confused with the Dig-Uno rollback image.
