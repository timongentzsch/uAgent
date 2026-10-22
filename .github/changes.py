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


SWITCHES = ("UAGENT_WEB", "UAGENT_BROWSER")  # also matches UAGENT_WEB_PUSH
HEAVY_DIRS = (
    "src/web/",
    "src/browser/",
    "include/web/",
    "include/browser/",
    ".github/",
    "cmake/",
    "third_party/",
)


def heavy(paths, old, new):
    """Whether a pull request also runs the legs that master always runs:
    ThreadSanitizer, Linux ARM64 and the CLI-only and no-browser variants.
    They matter for the build, the workflows and any source that a feature
    switch compiles differently, before or after the change: `old(path)` and
    `new(path)` give a file's text on each side, empty where it does not exist."""
    return any(
        path.startswith(HEAVY_DIRS)
        or path in ("CMakeLists.txt", "CMakePresets.json")
        or (
            path.endswith((".cc", ".h")) and any(name in old(path) + new(path) for name in SWITCHES)
        )
        for path in paths
    )


def build_matrix(with_arm):
    """The Release builds of build-and-test; ARM64 only where it is run."""
    legs = [
        {"target": "linux-x86_64", "runner": "ubuntu-24.04", "arch": "x86_64"},
        {"target": "linux-arm64", "runner": "ubuntu-24.04-arm", "arch": "aarch64"},
        {"target": "macos-arm64", "runner": "macos-15", "arch": "arm64"},
    ]
    return json.dumps({"include": legs if with_arm else [legs[0], legs[2]]})


def tidy(paths):
    """What clang-tidy reads on a pull request: the changed translation
    units, or "all" when a header, the build or the rules changed, since any
    unit may include or obey those. Master reads everything."""
    wide = any(
        path.endswith((".h", ".hpp", ".clang-tidy"))
        or path.startswith((".github/", "third_party/"))
        or path in ("CMakeLists.txt", "CMakePresets.json")
        for path in paths
    )
    units = [
        path
        for path in paths
        if path.endswith(".cc") and path.startswith(("src/", "tests/", "benchmarks/"))
    ]
    return "all" if wide else " ".join(units)


def tidy_files(entries, units, shard="0/1"):
    """The run-clang-tidy.py patterns for what tidy() chose, from the entries
    of compile_commands.json. "all" deals the first-party units round robin
    and keeps the `shard` ("index/count") share; a changed unit this build
    does not compile is not in the list."""
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
        # No rename detection: both the old and the new path are listed.
        # Against the working tree, which in CI is HEAD and before a push
        # (.github/ci-local.sh) includes what is not committed yet.
        changed = subprocess.check_output(
            ["git", "diff", "--name-only", "--no-renames", "-z", base]
        )
        paths = [os.fsdecode(path) for path in changed.split(b"\0") if path]

        def old(path):
            done = subprocess.run(["git", "show", f"{base}:{path}"], capture_output=True)
            return done.stdout.decode(errors="replace")

        def new(path):
            try:
                return Path(path).read_text(errors="replace")
            except OSError:
                return ""

        is_heavy = heavy(paths, old, new)
        result = {**classify(paths), "tidy": tidy(paths), "heavy": is_heavy}
    else:
        is_heavy = True
        result = {"native": True, "web": True, "tidy": "all", "heavy": True}
    result["matrix"] = build_matrix(is_heavy)
    output = "".join(
        f"{name}={value if isinstance(value, str) else str(value).lower()}\n"
        for name, value in result.items()
    )
    if destination := os.environ.get("GITHUB_OUTPUT"):
        with Path(destination).open("a") as stream:
            stream.write(output)
    print(output, end="")
