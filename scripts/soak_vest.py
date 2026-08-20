#!/usr/bin/env python3
"""REST-health soak harness for Dig-Next-2 MoonLight stability checks."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request


def fetch(url: str, method: str = "GET", timeout: float = 10.0, data: bytes | None = None) -> tuple[int, float, str]:
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read(4096).decode("utf-8", errors="replace")
            return resp.status, (time.perf_counter() - start) * 1000.0, body
    except urllib.error.HTTPError as exc:
        body = exc.read(4096).decode("utf-8", errors="replace")
        return exc.code, (time.perf_counter() - start) * 1000.0, body
    except Exception as exc:  # noqa: BLE001 - soak harness reports all failures
        return 0, (time.perf_counter() - start) * 1000.0, str(exc)


def parse_status(body: str) -> dict:
    try:
        data = json.loads(body)
    except json.JSONDecodeError:
        return {}
    return data if isinstance(data, dict) else {}


def parse_dma_kb(body: str) -> int | None:
    data = parse_status(body)
    largest = data.get("largest_free_dma")
    if isinstance(largest, int):
        return largest // 1024
    dma_text = data.get("heap_info_dma", "")
    if isinstance(dma_text, str):
        match = re.search(r"🔹(\d+)KB", dma_text)
        if match:
            return int(match.group(1))
    match = re.search(r"🔹(\d+)KB", body)
    if match:
        return int(match.group(1))
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Check MoonLight REST health over time")
    parser.add_argument("--host", default="192.168.20.239", help="Vest IP or hostname")
    parser.add_argument("--minutes", type=float, default=15.0, help="Soak duration")
    parser.add_argument("--quick", action="store_true", help="Run a 2-minute smoke soak")
    parser.add_argument("--interval", type=float, default=5.0, help="Seconds between cycles")
    parser.add_argument(
        "--min-dma-kb",
        type=int,
        default=8,
        help="REST-health diagnostic threshold for the largest DMA block (KB)",
    )
    parser.add_argument("--bad-streak", type=int, default=3, help="Consecutive low-DMA cycles before fail")
    parser.add_argument("--firmware", default="dignext2.139", help="Required firmware version substring")
    parser.add_argument(
        "--require-preset-apply",
        action="store_true",
        help="Require lightscontrol selected preset to change during this run",
    )
    args = parser.parse_args()
    if args.quick:
        args.minutes = 2.0
        args.interval = 3.0

    base = f"http://{args.host}"
    end = time.time() + args.minutes * 60
    cycle = 0
    failures: list[str] = []
    low_dma_streak = 0
    last_uptime: int | None = None
    first_selected: int | None = None
    last_selected: int | None = None
    observed_preset_apply = False

    print(
        f"REST health {base} for {args.minutes} min, interval {args.interval}s, "
        f"diagnostic min DMA {args.min_dma_kb}KB"
    )

    while time.time() < end:
        cycle += 1
        status, ms, body = fetch(f"{base}/rest/systemStatus", timeout=15)
        data = parse_status(body)
        dma = parse_dma_kb(body)
        uptime = data.get("uptime")
        firmware = data.get("firmware_version")

        if status != 200:
            failures.append(f"cycle {cycle}: systemStatus HTTP {status} ({ms:.0f}ms)")
        elif not isinstance(firmware, str) or args.firmware not in firmware:
            failures.append(f"cycle {cycle}: firmware {firmware!r} missing {args.firmware!r}")
        elif uptime is None:
            failures.append(f"cycle {cycle}: systemStatus missing uptime")
        elif dma is None:
            failures.append(f"cycle {cycle}: systemStatus missing DMA telemetry")
        elif dma < args.min_dma_kb:
            low_dma_streak += 1
            if low_dma_streak >= args.bad_streak:
                failures.append(f"cycle {cycle}: DMA {dma}KB below {args.min_dma_kb}KB for {low_dma_streak} cycles")
        else:
            low_dma_streak = 0

        if isinstance(uptime, int) and last_uptime is not None and uptime < last_uptime - 5:
            failures.append(f"cycle {cycle}: reboot detected (uptime {last_uptime} -> {uptime})")
        if isinstance(uptime, int):
            last_uptime = uptime

        lights_status, lights_ms, lights_body = fetch(f"{base}/rest/lightscontrol", timeout=20)
        lights_data = parse_status(lights_body)
        selected = lights_data.get("preset", {}).get("selected") if isinstance(lights_data.get("preset"), dict) else None
        if lights_status != 200:
            failures.append(f"cycle {cycle}: /rest/lightscontrol HTTP {lights_status} ({lights_ms:.0f}ms)")

        if isinstance(selected, int):
            if first_selected is None:
                first_selected = selected
            if last_selected is not None and selected != last_selected:
                observed_preset_apply = True
            last_selected = selected

        for path in (
            "/moonbase/module?group=moonlight&module=lightscontrol",
            "/moonbase/module?group=moonlight&module=drivers",
            "/rest/monitor",
        ):
            code, elapsed, _ = fetch(f"{base}{path}", timeout=20)
            if code != 200:
                failures.append(f"cycle {cycle}: {path} HTTP {code} ({elapsed:.0f}ms)")
            elif elapsed > 5000:
                failures.append(f"cycle {cycle}: {path} slow {elapsed:.0f}ms")

        # Direct REST preset apply is intentionally disabled; this is only a REST-health check.
        payload = json.dumps({"preset": {"action": "click", "select": 1}}).encode()
        code, elapsed, _ = fetch(f"{base}/rest/lightscontrol", method="POST", data=payload, timeout=10)
        if code != 409:
            failures.append(f"cycle {cycle}: REST preset click expected 409, got {code} ({elapsed:.0f}ms)")

        print(
            f"cycle {cycle}: status={status} dma={dma}KB uptime={uptime}s "
            f"selected={selected} low_dma_streak={low_dma_streak} failures={len(failures)}"
        )

        if failures:
            break
        time.sleep(args.interval)

    if args.require_preset_apply and not observed_preset_apply:
        failures.append("preset-apply gate requested but lightscontrol selected preset never changed")

    if failures:
        print("REST HEALTH FAIL")
        for item in failures[:20]:
            print(f" - {item}")
        return 1

    gate = "preset-apply observed" if observed_preset_apply else "preset-apply not gated"
    print(f"REST HEALTH PASS ({cycle} cycles; {gate})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
