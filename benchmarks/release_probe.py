#!/usr/bin/env python3
"""Measure one installed µAgent release against a scripted loopback provider."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from compare_agents import PROMPT

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tests"))
from integration_support import Server, base_env, event  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--machine", help="machine label in the footprint report")
    parser.add_argument("--request-only", action="store_true", help="skip the footprint run")
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with (
        tempfile.TemporaryDirectory(dir="/tmp") as tmp,
        Server([lambda *_: event({"content": "ok"})]) as provider,
    ):
        root = Path(tmp)
        workspace = root / "project"
        workspace.mkdir()

        def environment(home):
            env = base_env(home, provider.url)
            return {
                **{key: val for key, val in env.items() if key.startswith("UAGENT_")},
                "HOME": str(home),
                "PATH": os.defpath,
                "TERM": "xterm-256color",
                "SHELL": "/bin/bash",
                "TZ": "UTC",
            }

        first = root / "first"
        first.mkdir()
        turn = subprocess.run(
            [str(binary), "-p", PROMPT, "--json"],
            cwd=workspace,
            env=environment(first),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            check=True,
            timeout=60,
        )
        if json.loads(turn.stdout).get("answer") != "ok":
            raise RuntimeError("expected the scripted one-word answer")
        if len(provider.requests) != 1 or not provider.requests[0][1].get("tools"):
            raise RuntimeError("expected one model request carrying tool definitions")
        headers, request = provider.requests[0]
        # Keep the original wire size and the unmodified model input for recounting.
        body = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode()
        if len(body) != int(headers["Content-Length"]):
            raise RuntimeError("captured JSON length differs from the wire body")
        (output / "first-request.json").write_bytes(body)
        if args.request_only:
            return
        home = root / "footprint"
        home.mkdir()
        subprocess.run(
            [
                sys.executable,
                str(HERE / "compare_agents.py"),
                "--uagent",
                str(binary),
                "--scripted-provider",
                *(["--machine", args.machine] if args.machine else []),
                "--output",
                str(output / "footprint.json"),
            ],
            cwd=workspace,
            env=environment(home),
            check=True,
        )


if __name__ == "__main__":
    main()
