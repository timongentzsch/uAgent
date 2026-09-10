#!/usr/bin/env python3
"""Measure native web footprint using disposable state and a mock provider."""

import argparse
import gzip
import json
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import time

SOURCE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tests"))
from integration_support import Server, event  # noqa: E402
from web_support import web_host  # noqa: E402


def cpu_seconds(value):
    result = 0.0
    for field in value.split(":"):
        result = result * 60 + float(field)
    return result


def process_sample(pid):
    values = subprocess.check_output(["ps", "-p", str(pid), "-o", "rss=,time="], text=True).split()
    thread_flag = "-M" if sys.platform == "darwin" else "-L"
    threads = subprocess.check_output(
        ["ps", thread_flag, "-p", str(pid), "-o", "pid="], text=True
    ).splitlines()
    return {
        "rss_kib": int(values[0]),
        "cpu_seconds": cpu_seconds(values[1]),
        "threads": len(threads),
    }


def children(pid):
    rows = subprocess.check_output(["ps", "-ax", "-o", "pid=,ppid="], text=True).splitlines()
    return [int(row.split()[0]) for row in rows if int(row.split()[1]) == pid]


def stripped_bytes(binary):
    with tempfile.TemporaryDirectory(prefix="uagent-strip-") as directory:
        copy = pathlib.Path(directory) / "uagent"
        shutil.copyfile(binary, copy)
        subprocess.run(["strip", str(copy)], check=True, capture_output=True)
        return copy.stat().st_size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--cli-only", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument(
        "--push-contact", help="enable the native sender; no subscriptions are registered"
    )
    args = parser.parse_args()
    report = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "binary_bytes": args.binary.stat().st_size,
        "stripped_bytes": stripped_bytes(args.binary),
    }
    for name, path in (("baseline_bytes", args.baseline), ("cli_only_bytes", args.cli_only)):
        if path:
            report[name] = path.stat().st_size
            report[name.replace("_bytes", "_stripped_bytes")] = stripped_bytes(path)
    assets = {}
    for path in sorted((SOURCE / "web/dist").rglob("*")):
        if path.is_file():
            data = path.read_bytes()
            assets[str(path.relative_to(SOURCE / "web/dist"))] = {
                "raw": len(data),
                "gzip": len(gzip.compress(data, compresslevel=6, mtime=0)),
            }
    report["assets"] = assets
    report["asset_totals"] = {
        kind: sum(value[kind] for value in assets.values()) for kind in ("raw", "gzip")
    }
    with (
        tempfile.TemporaryDirectory(prefix="uagent-web-metrics-") as temporary,
        Server([lambda _, _body: event({"content": "Measured response"})]) as provider,
    ):
        root = pathlib.Path(temporary)
        home = root / "home"
        home.mkdir()
        with web_host(
            args.binary.resolve(),
            root,
            home,
            provider.url,
            extra_env={"UAGENT_WEB_PUSH_CONTACT": args.push_contact},
        ) as (
            client,
            code,
            process,
            _,
        ):
            client.pair(code)
            before = process_sample(process.pid)
            started = time.monotonic()
            time.sleep(3)
            after = process_sample(process.pid)
            report["idle_master"] = after | {
                "sample_seconds": round(time.monotonic() - started, 3),
                "cpu_delta_seconds": round(after["cpu_seconds"] - before["cpu_seconds"], 3),
            }
            sessions = []
            for index in range(2):
                project = root / f"project-{index}"
                project.mkdir()
                sessions.append(client.create(project))
            for session in sessions:
                client.command("submit", session, text="Measure this worker")
            for session in sessions:
                client.until(
                    session,
                    lambda value: (
                        value["metadata"]["status"] == "idle"
                        and bool(value["state"].get("view", {}).get("blocks"))
                    ),
                )
            time.sleep(1)
            report["master_with_two_workers"] = process_sample(process.pid)
            report["two_workers"] = [process_sample(pid) for pid in children(process.pid)]
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "assets"}, indent=2))


if __name__ == "__main__":
    main()
