#!/usr/bin/env python3
"""Render the request-encoding SVG from a dated measurement snapshot."""

import argparse
import json
from pathlib import Path

from charts import bars


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/images"))
    arguments = parser.parse_args()
    snapshot = json.loads(arguments.snapshot.read_text())
    arguments.output.mkdir(parents=True, exist_ok=True)
    notes = [f"{snapshot['date']} · measured commit {snapshot['measured_commit'][:8]}"]
    core = snapshot["local"]["core"]
    (arguments.output / "request-encoding.svg").write_text(
        bars(
            "Request payload serialization",
            [
                (
                    f"{kind.title()} payload, ms",
                    [
                        (label, core[f"{route} {kind} payload"]["median_ms"], kind == "cached")
                        for label, route in (
                            ("Chat Completions", "chat_completions"),
                            ("Responses", "responses"),
                            ("Anthropic Messages", "anthropic_messages"),
                        )
                    ],
                    1,
                )
                for kind in ("full", "cached")
            ],
            notes
            + [
                f"1,000 serializations · ms · median of {snapshot['local']['core_runs']} Release runs · {snapshot['local']['machine']}",
                "Shared zero-based scale · local CPU work; excludes provider and network latency",
            ],
            shared_scale=True,
            label_width=158,
        )
    )


if __name__ == "__main__":
    main()
