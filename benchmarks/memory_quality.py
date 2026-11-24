#!/usr/bin/env python3
"""Opt-in paired memory evaluation using a configured live provider and synthetic data."""

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from integration_support import run_pty, wait_until, write_session  # noqa: E402
from run_trace import read_trace, trace_metrics  # noqa: E402
from session_support import remove_runtime, stop_sessions  # noqa: E402

OLD = "MANGO-714"
NEW = "PEAR-683"
DETAIL = "TULIP-4729"
KEY = "project/release-review-label"
HISTORICAL_PREFERENCES = (
    f"For release review in this repository, always use the checklist label {NEW}. Please remember that across future tasks.",
    f"My durable preference is to label every release review in this repository {NEW}.",
    f"Every release-review checklist in this project should carry {NEW}. Make this the standing convention.",
    f"Use {NEW} as the standing release-review label in this repository.",
)


def provider_environment(model):
    settings = Path.home() / ".uagent/config/settings.json"
    configured = json.loads(settings.read_text()).get("all", {}) if settings.is_file() else {}
    routes = (
        json.loads(os.environ["UAGENT_PROVIDERS"])
        if "UAGENT_PROVIDERS" in os.environ
        else configured.get("providers", {})
    )
    name = model.split("/", 1)[0]
    if name not in routes:
        raise ValueError(f"Configure the provider named {name} before running this evaluation")
    route = routes[name].copy()
    key = route.get("api_key", "")
    ref = re.fullmatch(r"\$\{(\w+)\}|\$(\w+)|env:(\w+)", key)
    env = {
        k: os.environ[k]
        for k in ("PATH", "LANG", "SSL_CERT_DIR", "SSL_CERT_FILE")
        if k in os.environ
    }
    if ref:
        variable = next(group for group in ref.groups() if group)
        route["api_key"] = os.environ.get(variable) or configured.get("variables", {}).get(variable)
        if not route["api_key"]:
            raise ValueError(f"Missing configured credential for {name}")
    env.update(
        UAGENT_PROVIDERS=json.dumps({name: route}),
        UAGENT_MODEL=model,
        UAGENT_TITLE_MODEL="off",
        UAGENT_MAX_STEPS="8",
        UAGENT_MAX_TOKENS="1000",
        UAGENT_READ_FILE_KB="1",
        UAGENT_READ_FILE_LINES="6",
        UAGENT_MAX_TURN_SECONDS="100",
    )
    return env


def arm(binary, env, enabled, deletion_probe=None):
    with tempfile.TemporaryDirectory(prefix="uagent-memory-quality-") as temp:
        root = Path(temp).resolve()
        home, work, other, scratch = (root / name for name in ("home", "project", "other", "tmp"))
        for p in (home, work, other, scratch):
            p.mkdir()
        env = {**env, "HOME": str(home), "TMPDIR": str(scratch), "TERM": "xterm-256color"}
        flags = [] if enabled else ["--no-memory"]

        def call(cwd, args):
            p = subprocess.run(
                [str(binary), *args],
                cwd=cwd,
                env={**env, "UAGENT_MEMORY_GENERATE": "0"},
                capture_output=True,
                text=True,
                timeout=120,
            )
            if p.returncode:
                # The provider's error and its request may include credentials.
                raise RuntimeError(f"uagent {args[0]} exited {p.returncode}")
            return json.loads(p.stdout)

        def manage(action, **fields):
            return call(
                work, ["--control", json.dumps({"kind": "memory", "action": action, **fields})]
            )

        def seed(name, preference):
            dialogue = (
                [
                    preference,
                    "Understood. I will apply that preference to future reviews.",
                    "All future releases in this project should follow that convention.",
                    "Understood.",
                ]
                if deletion_probe is not None
                else [
                    f"Standing repository preference: {preference}",
                    "I will use that review label in future releases.",
                    "This applies to future releases of this project only.",
                    "Understood.",
                ]
            )
            p = write_session(
                home,
                name,
                [
                    {"role": role, "content": line}
                    for role, line in zip(
                        ("user", "assistant", "user", "assistant"), dialogue, strict=True
                    )
                ],
                cwd=work,
                turns=2,
            )
            then = time.time() - 8 * 3600
            os.utime(p, (then, then))

        def extract(label):
            trace = root / f"{label}.jsonl"

            def finished():
                wait_until(
                    lambda: any(
                        r.get("event") == "memory_extract_finished" for r in read_trace(trace)
                    ),
                    "no extraction receipt",
                    timeout=95,
                )

            code, _ = run_pty(
                work,
                {**env, "UAGENT_MEMORY_GENERATE": "1"},
                b"/q\n",
                binary=binary,
                args=(*flags, "--budget", "0.10", f"--debug={trace}"),
                before_payload=finished if enabled else None,
                startup_marker=b"Ready",
                timeout=110,
            )
            records = read_trace(trace)
            actions = [
                {"action": r["data"]["action"], "key": r["data"].get("key", "")}
                for r in records
                if r.get("event") == "memory_extract_finished"
            ]
            if code or any(a["action"] == "failed" for a in actions):
                raise RuntimeError("live memory extraction failed")
            return {"completed_passes": len(actions), "receipts": actions}

        def query(label, cwd, prompt):
            trace = root / f"{label}.jsonl"
            reply = call(
                cwd, ["--json", "--budget", "0.10", *flags, f"--debug={trace}", "-p", prompt]
            )
            records = read_trace(trace)
            metrics = trace_metrics(records)
            result = {
                "model_requests": metrics["model_requests"],
                "tools": [r["data"]["name"] for r in records if r.get("event") == "tool_start"],
            }

            if label == "recovery":
                result["read_ranges"] = [
                    {
                        "fixture_file": Path(call["arguments"].get("path", "")).name
                        == "dispatch.txt",
                        "offset": call["arguments"].get("offset", 1),
                        "limit": call["arguments"].get("limit", 0),
                    }
                    for call in metrics["calls"]
                    if call["name"] == "read_path"
                ]
                reads = [
                    r["data"]
                    for r in records
                    if r.get("event") == "tool_result"
                    and r["data"].get("name") in ("read_path", "grep")
                ]
                result["initial_read_omitted_detail"] = bool(
                    result["tools"][:1] == ["read_path"]
                    and reads
                    and reads[0]["name"] == "read_path"
                    and DETAIL not in reads[0].get("result", "")
                    and "02: pending" in reads[0].get("result", "")
                )
                result["later_read_contained_detail"] = any(
                    DETAIL in d.get("result", "") for d in reads[1:]
                )
            return reply.get("answer", "").strip(), result

        def matching_keys(token):
            return [
                i["key"]
                for i in manage("list")["items"]
                if token in manage("get", key=i["key"])["item"]["content"]
            ]

        question = (
            "What exact release-review label do I prefer for this repository? Consult only "
            "saved project memory. If it does not say, reply UNKNOWN. Reply with just the label."
        )
        try:
            if deletion_probe is not None:
                manage(
                    "set",
                    key=KEY,
                    revision="",
                    content=f"Use {NEW} as the release-review label in this repository.",
                )
                seed("past", HISTORICAL_PREFERENCES[deletion_probe])
                manage("forget", key=KEY, revision=manage("get", key=KEY)["item"]["revision"])
                return {
                    "wording": deletion_probe + 1,
                    **extract("deletion"),
                    "restored_keys": matching_keys(NEW),
                }
            manage(
                "set",
                key=KEY,
                revision="",
                content=f"Use {OLD} as this repository's release-review label.",
            )
            seed(
                "correction",
                f"Replace the release-review label {OLD} with {NEW}. Always use {NEW} for this repository.",
            )
            first = extract("correction")
            persisted = {"old": matching_keys(OLD), "new": matching_keys(NEW)}
            answer, recall = query("recall", work, question)
            foreign, isolation = query("isolation", other, question)
            # A second historical conversation becomes eligible after the user forgets.
            # Its content predates deletion; no fresh user instruction reauthorizes it.
            deleted_at_start = [i["key"] for i in manage("list")["items"]]
            after_forget = []
            for case, preference in (
                ("historical-a", HISTORICAL_PREFERENCES[0]),
                ("historical-b", HISTORICAL_PREFERENCES[2]),
            ):
                seed(case, preference)
                for item in manage("list")["items"]:
                    manage("forget", key=item["key"], revision=item["revision"])
                empty = not manage("list")["items"]
                receipt = extract(case)
                after_forget.append(
                    {
                        "source": case,
                        "empty_after_manual_delete": empty,
                        "later_extraction": receipt,
                        "restored_keys": matching_keys(NEW),
                    }
                )
            lines = (
                ["Dispatch records; final_dispatch is on line 81."]
                + [f"{n:02}: pending" for n in range(2, 81)]
                + [f"final_dispatch = {DETAIL}"]
            )
            (work / "dispatch.txt").write_text("\n".join(lines) + "\n")
            exact, recovery = query(
                "recovery",
                work,
                "Open dispatch.txt with read_path using its default range first. "
                "What is the exact final_dispatch code? Verify it from the file, and reply with just the code.",
            )
            return {
                "mode": "on" if enabled else "off",
                "correction_extraction": first,
                "corrected_note": persisted,
                "recall": {
                    **recall,
                    "corrected": answer.upper() == NEW,
                    "correct_value_returned": NEW in answer.upper() and OLD not in answer.upper(),
                    "unknown": answer.upper() == "UNKNOWN",
                },
                "cross_project": {
                    **isolation,
                    "unknown": foreign.upper() == "UNKNOWN",
                    "leaked_token": NEW in foreign or OLD in foreign,
                },
                "deletion": {
                    "initially_deleted_keys": deleted_at_start,
                    "subsequent_passes": after_forget,
                },
                "shortened_read": {
                    **recovery,
                    "exact_value_returned": DETAIL in exact.upper(),
                    "bare_value": exact.upper() == DETAIL,
                },
            }
        finally:
            stop_sessions(home, settle=0.25)
            remove_runtime(home)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--model", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    env = provider_environment(args.model)
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda enabled: arm(binary, env, enabled), (True, False)))
        probes = list(
            pool.map(
                lambda index: arm(binary, env, True, index), range(len(HISTORICAL_PREFERENCES))
            )
        )
    report = {
        "date_utc": datetime.datetime.now(datetime.UTC).date().isoformat(),
        "model": args.model,
        "provider": "live",
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "fixture": "Synthetic conversations and notes, one independent HOME per mode. A model response is one sample.",
        "results": results,
        "independent_deletion_probes": probes,
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
