#!/usr/bin/env python3
"""Render the published size and request-encoding charts from a measured snapshot.

Run with `uv run --with matplotlib python benchmarks/plot.py SNAPSHOT.json`.
Matplotlib is a reporting dependency only; the agent and its tests do not need it.
"""

import argparse
import json
import xml.etree.ElementTree as ET
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def draw(path, panels, labels, caption):
    plt.rcParams.update({"svg.fonttype": "none", "font.size": 10})
    figure, _ = plt.subplots(1, len(panels), figsize=(max(7.8, 3.7 * len(panels)), 3.6))
    for axis, (title, series) in zip(figure.axes, panels, strict=True):
        width = 0.8 / len(series)
        for index, (name, values) in enumerate(series.items()):
            positions = [x + (index - (len(series) - 1) / 2) * width for x in range(len(labels))]
            bars = axis.bar(
                positions, values, width, label=name, color=("#0969da", "#8c959f")[index]
            )
            axis.bar_label(bars, fmt="%.1f", padding=3, fontsize=9)
        axis.set_xticks(range(len(labels)), labels)
        axis.set_title(title, loc="left", fontsize=11, fontweight="bold", pad=15)
        axis.set_ylim(0, max(max(values) for values in series.values()) * 1.2)
        axis.spines[["top", "right"]].set_visible(False)
        axis.spines[["left", "bottom"]].set_color("#d1d9e0")
        if len(series) > 1:
            axis.legend(frameon=False)
    figure.text(0.03, 0.015, caption, fontsize=9, color="#59636e")
    figure.tight_layout(rect=(0, 0.08, 1, 1))
    figure.savefig(path, format="svg", metadata={"Date": None})
    plt.close(figure)
    namespace = "http://www.w3.org/2000/svg"
    ET.register_namespace("", namespace)
    tree = ET.parse(path)
    tree.getroot().set("role", "img")
    ET.SubElement(tree.getroot(), f"{{{namespace}}}title").text = "; ".join(
        title for title, _ in panels
    )
    ET.SubElement(tree.getroot(), f"{{{namespace}}}desc").text = caption
    tree.write(path, encoding="unicode")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/images"))
    arguments = parser.parse_args()
    snapshot = json.loads(arguments.snapshot.read_text())
    arguments.output.mkdir(parents=True, exist_ok=True)
    packages = snapshot["packages"]
    labels = [
        row["target"]
        .replace("linux-", "Linux\n")
        .replace("macos-", "macOS\n")
        .replace("arm64", "ARM64")
        for row in packages
    ]
    draw(
        arguments.output / "footprint.svg",
        [
            (title, {"MB": [row[key] / 1e6 for row in packages]})
            for title, key in (
                ("Complete installation, MB", "complete_install_bytes"),
                ("Remote executable, MB", "remote_binary_bytes"),
                ("Remote transfer + resources, MB", "remote_transfer_bytes"),
            )
        ],
        labels,
        f"{snapshot['date']} · CI archives {snapshot['measured_commit'][:8]} · complete install includes all four gateways · MB = 1,000,000 bytes",
    )
    core = snapshot["local"]["core"]
    draw(
        arguments.output / "request-encoding.svg",
        [
            (
                "1,000 request payload serializations, ms",
                {
                    kind: [
                        core[f"{route} {kind} payload"]["median_ms"]
                        for route in ("chat_completions", "responses", "anthropic_messages")
                    ]
                    for kind in ("full", "cached")
                },
            )
        ],
        ["Chat Completions", "Responses", "Anthropic Messages"],
        f"{snapshot['date']} · median of 5 Release runs · {snapshot['local']['machine']} · local CPU work; no provider or network latency",
    )


if __name__ == "__main__":
    main()
