#!/usr/bin/env python3
"""Validate a µAgent binary archive and its bundled skill tree."""

import pathlib
import sys
import tarfile


def fail(message):
    raise SystemExit(f"invalid release archive: {message}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: package_contents.py ARCHIVE")
    archive = pathlib.Path(sys.argv[1])
    source = pathlib.Path(__file__).resolve().parents[1]
    expected_skills = set()
    for path in (source / "skills").rglob("*"):
        relative = path.relative_to(source / "skills")
        if path.is_file() and not any(part.startswith(".") for part in relative.parts):
            expected_skills.add(relative.as_posix())
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

    prefix = f"{root}/share/uagent/skills/"
    packaged_skills = {name.removeprefix(prefix) for name in files if name.startswith(prefix)}
    if packaged_skills != expected_skills:
        missing = sorted(expected_skills - packaged_skills)
        extra = sorted(packaged_skills - expected_skills)
        fail(f"skill tree differs; missing={missing}, extra={extra}")
    print(f"verified {archive}: {len(packaged_skills)} bundled skill files")


if __name__ == "__main__":
    main()
