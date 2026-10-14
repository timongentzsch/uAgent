"""Conservative job selection; unknown paths and non-PR runs get full coverage."""

import json
import os
import subprocess
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
            path.endswith((".cc", ".h"))
            and any(name in old(path) + new(path) for name in SWITCHES)
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
    include = [{**leg, "build_type": "Release"} for leg in legs]
    return json.dumps({"include": include if with_arm else [include[0], include[2]]})


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


if __name__ == "__main__":
    base = os.environ.get("CI_BASE_SHA")
    if os.environ.get("GITHUB_EVENT_NAME") == "pull_request" and base:
        # No rename detection: both the old and the new path are listed.
        changed = subprocess.check_output(
            ["git", "diff", "--name-only", "--no-renames", "-z", base, "HEAD"]
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
