#!/usr/bin/env python3
"""Validate a µAgent binary archive and its bundled skill tree."""

import argparse
import hashlib
import json
import pathlib
import tarfile


def fail(message):
    raise SystemExit(f"invalid release archive: {message}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("--local-only", action="store_true")
    args = parser.parse_args()
    archive = args.archive
    source = pathlib.Path(__file__).resolve().parents[1]
    expected_skills = set()
    for path in (source / "skills").rglob("*"):
        relative = path.relative_to(source / "skills")
        # The same two exclusions the install rule applies (CMakeLists.txt):
        # dotfiles, and the bytecode a developer leaves behind by running a
        # skill's script. Expecting a .pyc the archive deliberately omits fails
        # this check on a working tree and never in CI, which checks out clean.
        if not path.is_file() or any(
            part.startswith(".") or part == "__pycache__" for part in relative.parts
        ):
            continue
        expected_skills.add(relative.as_posix())
    # Installed from docs/ARCHITECTURE.md and the web's tokens rather than
    # duplicated in the tree.
    expected_skills.add("uagent-config/references/architecture.md")
    expected_skills.add("uagent-config/references/tokens.css")
    with tarfile.open(archive, "r:gz") as package:
        members = package.getmembers()
    if not members:
        fail("empty")

    roots = set()
    files = set()
    for member in members:
        path = pathlib.PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts or not path.parts:
            fail(f"unsafe path {member.name!r}")
        roots.add(path.parts[0])
        if member.issym() or member.islnk() or member.isdev():
            fail(f"unsupported member type at {member.name!r}")
        if member.isfile():
            files.add(path.as_posix())
    if len(roots) != 1:
        fail(f"expected one package root, found {sorted(roots)}")
    root = next(iter(roots))
    if f"{root}/bin/uagent" not in files:
        fail("bin/uagent is missing")

    docs = f"{root}/share/doc/uagent/"
    if f"{docs}THIRD_PARTY_NOTICES.md" not in files:
        fail("frontend licence notices are missing")
    for name in sorted(guide.stem for guide in (source / "docs").glob("*.md")):
        if f"{docs}docs/{name}.md" not in files:
            fail(f"canonical guide docs/{name}.md is missing")
        if f"{docs}{name}.md" in files:
            fail(f"duplicate flat guide {name}.md")

    prefix = f"{root}/share/uagent/skills/"
    packaged_skills = {name.removeprefix(prefix) for name in files if name.startswith(prefix)}
    if packaged_skills != expected_skills:
        missing = sorted(expected_skills - packaged_skills)
        extra = sorted(packaged_skills - expected_skills)
        fail(f"skill tree differs; missing={missing}, extra={extra}")
    # A sheet page fetches these: the engine, the code that draws a sheet with
    # it, and the engine's licences.
    for name in ("sheet.js", "sheet.css", "wasm.js", "wasm_bg.wasm", "THIRD-PARTY-NOTICES.md"):
        if f"{root}/share/uagent/sheet/{name}" not in files:
            fail(f"sheet engine file {name} is missing")
    if not args.local_only:
        manifests = [
            name
            for name in files
            if name.startswith(f"{root}/share/uagent/remote/") and name.endswith("/manifest.json")
        ]
        if len(manifests) != 1:
            fail("one complete remote bundle is required")
        with tarfile.open(archive, "r:gz") as package:
            manifest = json.load(package.extractfile(manifests[0]))
            targets = {"linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64"}
            if (
                set(manifest["targets"]) != targets
                or manifests[0].split("/")[-2] != manifest["build"]
            ):
                fail("remote targets or build identity differ")
            for target, record in manifest["targets"].items():
                name = manifests[0].removesuffix("manifest.json") + target + ".tar.gz"
                if name not in files:
                    fail(f"remote payload {target} missing")
                with package.extractfile(name) as payload:
                    if (
                        package.getmember(name).size != record["bytes"]
                        or hashlib.file_digest(payload, "sha256").hexdigest() != record["sha256"]
                    ):
                        fail(f"remote payload {target} integrity differs")
    print(f"verified {archive}: {len(packaged_skills)} bundled skill files")


if __name__ == "__main__":
    main()
