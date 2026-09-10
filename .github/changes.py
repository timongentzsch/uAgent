"""Conservative job selection; unknown paths and non-PR runs get full coverage."""

import os
import subprocess
from pathlib import Path


def classify(paths):
    docs = {"README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE"}
    native = any(not (path.startswith(("web/", "docs/")) or path in docs) for path in paths)
    return {"native": native, "web": native or any(path.startswith("web/") for path in paths)}


if __name__ == "__main__":
    base = os.environ.get("CI_BASE_SHA")
    if os.environ.get("GITHUB_EVENT_NAME") == "pull_request" and base:
        changed = subprocess.check_output(["git", "diff", "--name-only", "-z", base, "HEAD"])
        result = classify([os.fsdecode(path) for path in changed.split(b"\0") if path])
    else:
        result = {"native": True, "web": True}
    output = "".join(f"{name}={str(enabled).lower()}\n" for name, enabled in result.items())
    if destination := os.environ.get("GITHUB_OUTPUT"):
        with Path(destination).open("a") as stream:
            stream.write(output)
    print(output, end="")
