"""Conservative job selection; unknown paths and non-PR runs get full coverage."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path


def classify(paths):
    docs = {"README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE"}
    native = any(not (path.startswith(("web/", "docs/")) or path in docs) for path in paths)
    return {"native": native, "web": native or any(path.startswith("web/") for path in paths)}


def image(paths):
    """Whether a pull request builds the container image, which master always
    does: only when what defines it or publishes it changed."""
    return any(
        path.startswith(("deploy/", ".github/workflows/")) or path == ".dockerignore"
        for path in paths
    )


HEAVY_DIRS = (
    "src/web/",
    "src/browser/",
    "include/web/",
    "include/browser/",
    ".github/",
    "cmake/",
    "deploy/",
    "third_party/",
)


def heavy(paths):
    """Whether a pull request also runs ThreadSanitizer and Linux ARM64,
    which master always runs."""
    return any(
        path.startswith(HEAVY_DIRS)
        or path in ("CMakeLists.txt", "CMakePresets.json", ".dockerignore")
        for path in paths
    )


def bundle(paths):
    """Whether a pull request also builds Intel macOS and assembles the
    complete installation from all four builds, which master always does:
    only when what builds, packages or installs it changed. The other three
    builds cover that architecture and that system between them."""
    return any(
        path.startswith((".github/", "cmake/", "deploy/"))
        or path in ("CMakeLists.txt", "CMakePresets.json", ".dockerignore", "install.sh")
        for path in paths
    )


def build_matrix(with_arm, with_intel):
    """The Release builds of build-and-test: Linux ARM64 and Intel macOS only
    where they are run. Intel macOS is the slowest of them by far."""
    legs = [
        {"target": "linux-x86_64", "runner": "ubuntu-24.04", "arch": "x86_64"},
        {"target": "linux-arm64", "runner": "ubuntu-24.04-arm", "arch": "aarch64"},
        {"target": "macos-arm64", "runner": "macos-15", "arch": "arm64"},
        {"target": "macos-x86_64", "runner": "macos-15-intel", "arch": "x86_64"},
    ]
    chosen = [True, with_arm or with_intel, True, with_intel]
    return json.dumps({"include": [leg for leg, run in zip(legs, chosen, strict=True) if run]})


def tidy(paths):
    """What clang-tidy reads on a pull request: the changed units, or "all"
    when a header, the build or the rules changed. Master reads everything."""
    wide = any(
        path.endswith((".h", ".hpp", ".clang-tidy"))
        or path.startswith((".github/", "third_party/"))
        or path in ("CMakeLists.txt", "CMakePresets.json", ".dockerignore")
        for path in paths
    )
    units = [
        path
        for path in paths
        if path.endswith(".cc") and path.startswith(("src/", "tests/", "benchmarks/"))
    ]
    return "all" if wide else " ".join(units)


def tidy_files(entries, units, shard="0/1"):
    """The run-clang-tidy.py patterns for what tidy() chose. "all" deals the
    units round robin and keeps the `shard` ("index/count") share."""
    files = sorted(
        {
            entry["file"]
            for entry in entries
            if re.search(r"/(src|tests|benchmarks)/", entry["file"])
            and "/build/" not in entry["file"]
        }
    )
    if units == "all":
        index, count = map(int, shard.split("/"))
        files = files[index::count]
    else:
        files = [file for file in files if any(file.endswith("/" + unit) for unit in units.split())]
    return " ".join(re.escape(file) + "$" for file in files)


if __name__ == "__main__" and sys.argv[1:2] == ["--tidy-files"]:
    # --tidy-files BUILD_DIR UNITS [INDEX/COUNT]
    build, units, *shard = sys.argv[2:]
    entries = json.loads((Path(build) / "compile_commands.json").read_text())
    print(tidy_files(entries, units, *shard))
elif __name__ == "__main__":
    base = os.environ.get("CI_BASE_SHA")
    if os.environ.get("GITHUB_EVENT_NAME") == "pull_request" and base:
        # No rename detection: both the old and the new path are listed. The
        # working tree is compared, so ci-local.sh sees uncommitted changes.
        changed = subprocess.check_output(
            ["git", "diff", "--name-only", "--no-renames", "-z", base]
        )
        paths = [os.fsdecode(path) for path in changed.split(b"\0") if path]
        result = {
            **classify(paths),
            "tidy": tidy(paths),
            "heavy": heavy(paths),
            "bundle": bundle(paths),
            "image": image(paths),
        }
    else:
        result = {
            "native": True,
            "web": True,
            "tidy": "all",
            "heavy": True,
            "bundle": True,
            "image": True,
        }
    result["matrix"] = build_matrix(result["heavy"], result["bundle"])
    output = "".join(
        f"{name}={value if isinstance(value, str) else str(value).lower()}\n"
        for name, value in result.items()
    )
    if destination := os.environ.get("GITHUB_OUTPUT"):
        with Path(destination).open("a") as stream:
            stream.write(output)
    print(output, end="")
