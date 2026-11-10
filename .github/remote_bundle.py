"""Build and verify the four native payloads shipped with every remote-ready install."""

import argparse
import hashlib
import json
import pathlib
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile

TARGETS = {
    "linux-x86_64": "glibc 2.38",
    "linux-arm64": "glibc 2.38",
    "macos-x86_64": "macOS 15",
    "macos-arm64": "macOS 15",
}


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def verify_archive(path, record):
    if path.stat().st_size != record["bytes"] or digest(path) != record["sha256"]:
        raise ValueError(f"payload integrity differs: {path.name}")
    actual = {}
    with tarfile.open(path, "r:gz") as archive:
        for member in archive.getmembers():
            name = pathlib.PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts or not (member.isdir() or member.isfile()):
                raise ValueError(f"unsafe payload member: {member.name}")
            if member.isfile():
                if member.name in actual:
                    raise ValueError(f"duplicate payload member: {member.name}")
                with archive.extractfile(member) as source:
                    actual[member.name] = {
                        "bytes": member.size,
                        "sha256": hashlib.file_digest(source, "sha256").hexdigest(),
                    }
    if (
        actual != record["files"]
        or "uagent" not in actual
        or not any(name.startswith("share/uagent/skills/") for name in actual)
    ):
        raise ValueError(f"payload resources differ: {path.name}")


def payload(args):
    os_name = "macos" if platform.system() == "Darwin" else "linux"
    cpu = "arm64" if platform.machine() in ("arm64", "aarch64") else "x86_64"
    if args.target != f"{os_name}-{cpu}":
        raise ValueError("payload must be built and tested on its native target")
    build = subprocess.check_output([str(args.binary.resolve()), "--build-id"], text=True).strip()
    if len(build) != 64 or any(ch not in "0123456789abcdef" for ch in build):
        raise ValueError("invalid runtime build identity")
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        shutil.copy2(args.binary, root / "uagent")
        subprocess.run(["strip", str(root / "uagent")], check=True)
        if (
            subprocess.check_output([str(root / "uagent"), "--build-id"], text=True).strip()
            != build
        ):
            raise ValueError("stripped payload does not start with the expected identity")
        if os_name == "linux":
            versions = subprocess.check_output(
                ["readelf", "--version-info", str(root / "uagent")], text=True
            )
            required = [
                tuple(map(int, version.split(".")))
                for version in re.findall(r"GLIBC_([0-9.]+)", versions)
            ]
            if required and max(required) > (2, 38):
                raise ValueError(f"payload exceeds the glibc 2.38 floor: {max(required)}")
        else:
            commands = subprocess.check_output(["otool", "-l", str(root / "uagent")], text=True)
            minimum = re.findall(r"\bminos ([0-9.]+)", commands)
            if not minimum or any(
                tuple(map(int, version.split("."))) > (15, 0, 0) for version in minimum
            ):
                raise ValueError("payload exceeds the macOS 15 deployment floor")
        shutil.copytree(args.prefix / "share/uagent/skills", root / "share/uagent/skills")
        shutil.copytree(
            args.prefix / "share/doc/uagent/licenses", root / "share/doc/uagent/licenses"
        )
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copy2(args.prefix / "share/doc/uagent" / name, root / "share/doc/uagent" / name)
        files = {
            path.relative_to(root).as_posix(): {
                "bytes": path.stat().st_size,
                "sha256": digest(path),
            }
            for path in sorted(root.rglob("*"))
            if path.is_file()
        }
        path = args.out / f"{args.target}.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for file in sorted(files):
                archive.add(root / file, arcname=file, recursive=False)
    record = {
        "build": build,
        "target": args.target,
        "minimum": TARGETS[args.target],
        "bytes": path.stat().st_size,
        "sha256": digest(path),
        "files": files,
    }
    verify_archive(path, record)
    (args.out / f"{args.target}.json").write_text(json.dumps(record, indent=2) + "\n")


def assemble(args):
    records = {
        target: json.loads((args.input / f"{target}.json").read_text()) for target in TARGETS
    }
    builds = {record["build"] for record in records.values()}
    if len(builds) != 1:
        raise ValueError("the four payloads have different runtime identities")
    build = next(iter(builds))
    args.out.mkdir(parents=True, exist_ok=True)
    for target, record in records.items():
        if record["target"] != target or record["minimum"] != TARGETS[target]:
            raise ValueError(f"wrong native target or minimum: {target}")
        source = args.input / f"{target}.tar.gz"
        verify_archive(source, record)
        shutil.copy2(source, args.out / source.name)
    manifest = {"v": 1, "build": build, "targets": records}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if args.releases:
        for path in sorted(args.releases.glob("uagent-*.tar.gz")):
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                with tarfile.open(path) as archive:
                    archive.extractall(root, filter="data")
                package = next(root.iterdir())
                destination = package / "share/uagent/remote" / build
                shutil.copytree(args.out, destination)
                with tarfile.open(path, "w:gz") as archive:
                    archive.add(package, arcname=package.name)
            path.with_name(path.name + ".sha256").write_text(f"{digest(path)}  {path.name}\n")
    print(
        f"verified four-target bundle {build}: {sum(record['bytes'] for record in records.values())} compressed bytes"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("payload")
    one.add_argument("--prefix", type=pathlib.Path, required=True)
    one.add_argument("--binary", type=pathlib.Path, required=True)
    one.add_argument("--target", choices=TARGETS, required=True)
    one.add_argument("--out", type=pathlib.Path, required=True)
    all_targets = commands.add_parser("assemble")
    all_targets.add_argument("--input", type=pathlib.Path, required=True)
    all_targets.add_argument("--out", type=pathlib.Path, required=True)
    all_targets.add_argument("--releases", type=pathlib.Path)
    args = parser.parse_args()
    (payload if args.command == "payload" else assemble)(args)


if __name__ == "__main__":
    main()
