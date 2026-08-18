# Preset switching and vest effects

## Goal

Make preset switching safe on the Dig-Next-2, give presets honest labels, and install two performance patterns: wraparound racers and a crossing helix race.

## Root cause

Preset selection copies the chosen preset to `/.config/effects.json` and publishes that path through FileManager so the Effects module reloads it. FileManager's event and legacy WebSocket transports serialized the entire filesystem before checking for recipients. Under internal-memory pressure, the recursive scan aborted while opening another file.

## Changes

- Skip FileManager transport serialization only when that transport has no recipient.
- Keep initial subscribe/open snapshots and all actual FileManager clients unchanged.
- Allocate required transport snapshot JSON with the existing PSRAM-preferred allocator.
- Prefer an optional top-level preset `label`, retaining the existing first-node fallback.
- Add `Wraparound Racers` and `Crossing Helix Race` as device presets; remove the misleading duplicate `Lines` labels.

## Acceptance

1. Focused source/native checks and the exact `esp32-d0-pico2` build pass.
2. App-only flash verifies without changing NVS or LittleFS.
3. Repeated preset changes do not reset the controller, grow internal heap monotonically, or lose the monitor.
4. Both new presets render and move in the monitor; RMT remains GPIO2/95 and PDM samples advance.
5. Commit, review, merge, then verify exact merged firmware bytes and live health.
