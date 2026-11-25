"""Conservative job selection; unknown paths and non-PR runs get full coverage."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path


def classify(paths):
    docs = {"README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE"}
    # These files also ship with the worker and contribute to its build ID.
    worker_inputs = {
        "docs/architecture/overview.md",
        "web/public/design-helper.js",
        "web/public/deck-print.js",
        "web/src/shared/styles/tokens.css",
    }
    # docs/interfaces is checked against the binary (tests/reference_docs.py).
    native = any(
        path in worker_inputs
        or path.startswith("docs/interfaces/")
        or not (path.startswith(("web/", "docs/")) or path in docs)
        for path in paths
    )
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
)


def heavy(paths):
    """Whether a pull request also runs ThreadSanitizer and Linux ARM64,
    which the weekly run, a run by hand and a release always run."""
    return any(
        path.startswith(HEAVY_DIRS)
        or path in ("CMakeLists.txt", "CMakePresets.json", ".dockerignore")
        for path in paths
    )


def bundle(paths):
    """Whether a pull request also assembles the complete installation from
    all three builds, which master always does: only when what builds,
    packages or installs it changed."""
    return any(
        path.startswith((".github/", "cmake/", "deploy/"))
        or path in ("CMakeLists.txt", "CMakePresets.json", ".dockerignore", "install.sh")
        for path in paths
    )


def build_matrix(with_arm):
    """The Release builds of build-and-test: Linux ARM64 only where it is
    run."""
    legs = [
        {"target": "linux-x86_64", "runner": "ubuntu-24.04", "arch": "x86_64"},
        {"target": "linux-arm64", "runner": "ubuntu-24.04-arm", "arch": "aarch64"},
        {"target": "macos-arm64", "runner": "macos-15", "arch": "arm64"},
    ]
    chosen = [True, with_arm, True]
    return json.dumps({"include": [leg for leg, run in zip(legs, chosen, strict=True) if run]})


def tidy(paths):
    """What clang-tidy reads on a pull request or a push to master: the changed
    units, or "all" when a header, the build or the rules changed. The weekly
    run, a run by hand and a release read everything."""
    wide = any(
        path.endswith((".h", ".hpp", ".clang-tidy"))
        or path.startswith(".github/")
        or path in ("CMakeLists.txt", "CMakePresets.json", ".dockerignore")
        for path in paths
    )
    if wide:
        return "all"
    units = [
        path
        for path in paths
        if path.endswith(".cc") and path.startswith(("src/", "tests/", "benchmarks/"))
    ]
    return json.dumps(units) if units else ""


def tidy_files(entries, units, shard="0/1"):
    """One file-selection regex for run-clang-tidy.py. "all" deals the
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
        suffixes = tuple("/" + unit for unit in json.loads(units or "[]"))
        files = [file for file in files if file.endswith(suffixes)]
    return "|".join(re.escape(file) + "$" for file in files)


def selection(event, paths):
    """What a run of `event` runs, from the paths it changed (None where they
    are not known). A pull request runs what its paths need. A push to master
    builds and packages everything, as a release promotes its run, but leaves
    ThreadSanitizer and the reading of unchanged code to the pull request, the
    weekly run and a run by hand, which run everything."""
    if event == "pull_request" and paths is not None:
        return {
            **classify(paths),
            "tidy": tidy(paths),
            "heavy": heavy(paths),
            "bundle": bundle(paths),
            "image": image(paths),
        }
    everything = {
        "native": True,
        "web": True,
        "tidy": "all",
        "heavy": True,
        "bundle": True,
        "image": True,
    }
    if event == "push" and paths is not None:
        return {**everything, "tidy": tidy(paths), "heavy": False}
    return everything


def changed_paths(base, cwd=None):
    """Include unstaged and untracked sources in local checks. A rename selects
    the checks for both its old and its new path."""
    paths = subprocess.check_output(
        ["git", "diff", "--name-only", "--no-renames", "-z", base], cwd=cwd
    )
    paths += subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=cwd
    )
    return [os.fsdecode(path) for path in paths.split(b"\0") if path]


if __name__ == "__main__" and sys.argv[1:2] == ["--tidy-files"]:
    # --tidy-files BUILD_DIR UNITS [INDEX/COUNT]
    build, units, *shard = sys.argv[2:]
    entries = json.loads((Path(build) / "compile_commands.json").read_text())
    print(tidy_files(entries, units, *shard))
elif __name__ == "__main__":
    base = os.environ.get("CI_BASE_SHA", "")
    # The paths changed since the base: a pull request's, or the commit a
    # push to master followed when this clone has it (not after a force push).
    known = base.strip("0") and (
        subprocess.run(["git", "cat-file", "-e", f"{base}^{{commit}}"]).returncode == 0
    )
    paths = changed_paths(base) if known else None
    result = selection(os.environ.get("GITHUB_EVENT_NAME", ""), paths)
    result["matrix"] = build_matrix(result["heavy"] or result["bundle"])
    output = "".join(
        f"{name}={value if isinstance(value, str) else str(value).lower()}\n"
        for name, value in result.items()
    )
    if destination := os.environ.get("GITHUB_OUTPUT"):
        with Path(destination).open("a") as stream:
            stream.write(output)
    print(output, end="")
