#!/usr/bin/env python3
"""HTTP soak harness for Dig-Next-2 MoonLight vest stability checks."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request


def fetch(url: str, method: str = "GET", timeout: float = 10.0) -> tuple[int, float, str]:
    req = urllib.request.Request(url, method=method)
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


def parse_dma_kb(body: str) -> int | None:
    try:
        data = json.loads(body)
        dma_text = data.get("heap_info_dma", "")
        if isinstance(dma_text, str):
            match = re.search(r"🔹(\d+)KB", dma_text)
            if match:
                return int(match.group(1))
    except json.JSONDecodeError:
        pass
    match = re.search(r"🔹(\d+)KB", body)
    if not match:
        return None
    return int(match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser(description="Soak-test MoonLight vest over HTTP")
    parser.add_argument("--host", default="192.168.20.239", help="Vest IP or hostname")
    parser.add_argument("--minutes", type=float, default=15.0, help="Soak duration")
    parser.add_argument("--quick", action="store_true", help="Run a 2-minute smoke soak")
    parser.add_argument("--interval", type=float, default=5.0, help="Seconds between cycles")
    parser.add_argument("--min-dma-kb", type=int, default=4, help="Fail if DMA largest block drops below this")
    parser.add_argument("--bad-streak", type=int, default=3, help="Consecutive low-DMA cycles before fail")
    args = parser.parse_args()
    if args.quick:
        args.minutes = 2.0
        args.interval = 3.0

    base = f"http://{args.host}"
    end = time.time() + args.minutes * 60
    cycle = 0
    failures: list[str] = []
    low_dma_streak = 0
    last_uptime = None

    print(f"Soak {base} for {args.minutes} min, interval {args.interval}s, min DMA {args.min_dma_kb}KB")

    while time.time() < end:
        cycle += 1
        status, ms, body = fetch(f"{base}/rest/systemStatus", timeout=15)
        dma = parse_dma_kb(body)
        uptime = None
        try:
            uptime = json.loads(body).get("uptime")
        except json.JSONDecodeError:
            pass

        if status != 200:
            failures.append(f"cycle {cycle}: systemStatus HTTP {status} ({ms:.0f}ms)")
        elif dma is not None and dma < args.min_dma_kb:
            low_dma_streak += 1
            if low_dma_streak >= args.bad_streak:
                failures.append(f"cycle {cycle}: DMA {dma}KB below {args.min_dma_kb}KB for {low_dma_streak} cycles")
        else:
            low_dma_streak = 0

        if uptime is not None and last_uptime is not None and uptime < last_uptime - 5:
            failures.append(f"cycle {cycle}: reboot detected (uptime {last_uptime} -> {uptime})")
        if uptime is not None:
            last_uptime = uptime

        for path in (
            "/moonbase/module?group=moonlight&module=lightscontrol",
            "/moonbase/module?group=moonlight&module=drivers",
            "/rest/monitor",
        ):
            code, elapsed, _ = fetch(f"{base}{path}", timeout=20)
            if code != 200:
                failures.append(f"cycle {cycle}: {path} HTTP {code} ({elapsed:.0f}ms)")

        for preset in (1, 3, 5, 8, 1):
            payload = json.dumps({"preset": {"action": "click", "select": preset}}).encode()
            req = urllib.request.Request(
                f"{base}/rest/lightscontrol",
                data=payload,
                method="POST",
                headers={"Content-Type": "application/json"},
            )
            start = time.perf_counter()
            try:
                with urllib.request.urlopen(req, timeout=15) as resp:
                    if resp.status != 200:
                        failures.append(f"cycle {cycle}: preset {preset} HTTP {resp.status}")
            except Exception as exc:  # noqa: BLE001
                failures.append(f"cycle {cycle}: preset {preset} error {exc}")
            else:
                _ = (time.perf_counter() - start) * 1000.0

        print(
            f"cycle {cycle}: status={status} dma={dma}KB uptime={uptime}s "
            f"low_dma_streak={low_dma_streak} failures={len(failures)}"
        )

        if failures:
            break
        time.sleep(args.interval)

    if failures:
        print("SOAK FAIL")
        for item in failures[:20]:
            print(f" - {item}")
        return 1

    print(f"SOAK PASS ({cycle} cycles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
