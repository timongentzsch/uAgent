"""Conservative job selection; unknown paths and non-PR runs get full coverage."""

import os
import subprocess
from pathlib import Path


def classify(paths):
    docs = {"README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE"}
    native = any(not (path.startswith(("web/", "docs/")) or path in docs) for path in paths)
    return {"native": native, "web": native or any(path.startswith("web/") for path in paths)}


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
        changed = subprocess.check_output(["git", "diff", "--name-only", "-z", base, "HEAD"])
        paths = [os.fsdecode(path) for path in changed.split(b"\0") if path]
        result = {**classify(paths), "tidy": tidy(paths)}
    else:
        result = {"native": True, "web": True, "tidy": "all"}
    output = "".join(
        f"{name}={value if isinstance(value, str) else str(value).lower()}\n"
        for name, value in result.items()
    )
    if destination := os.environ.get("GITHUB_OUTPUT"):
        with Path(destination).open("a") as stream:
            stream.write(output)
    print(output, end="")
