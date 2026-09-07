#!/usr/bin/env python3
import argparse
import os
import pathlib
import sys
import tempfile
import time

import integration_delegation
import integration_mcp
import integration_providers
import integration_runtime
import integration_sandbox
import integration_tools
import integration_ui

TEST_MODULES = (
    ("runtime", integration_runtime),
    ("tools", integration_tools),
    ("ui", integration_ui),
    ("providers", integration_providers),
    ("mcp", integration_mcp),
    ("delegation", integration_delegation),
    ("sandbox", integration_sandbox),
)
ALL_TESTS = {
    name: test
    for _, module in TEST_MODULES
    for name, test in vars(module).items()
    if name.startswith("test_")
    and callable(test)
    and getattr(test, "__module__", None) == module.__name__
}
TEST_GROUPS = {
    name: group
    for group, module in TEST_MODULES
    for name, test in ALL_TESTS.items()
    if test.__module__ == module.__name__
}
ORDERED_TESTS = tuple(ALL_TESTS)


def parse_args():
    parser = argparse.ArgumentParser(description="µAgent integration suite")
    parser.add_argument("binary", type=pathlib.Path, help="uagent binary under test")
    parser.add_argument("--group", default="all", help="CTest shard: runtime, tools, ui, ...")
    parser.add_argument("--test", action="append", default=[], help="exact test name; repeatable")
    parser.add_argument(
        "-k", "--match", action="append", default=[], help="substring filter; repeatable"
    )
    parser.add_argument("--list", action="store_true", help="print the selection and exit")
    return parser.parse_args()


def select(arguments):
    names = [
        name
        for name in ORDERED_TESTS
        if arguments.group == "all" or TEST_GROUPS[name] == arguments.group
    ]
    if arguments.test:
        unknown = sorted(set(arguments.test) - set(ALL_TESTS))
        if unknown:
            raise SystemExit(f"unknown test name: {unknown}")
        names = [name for name in names if name in set(arguments.test)]
    if arguments.match:
        names = [name for name in names if any(part in name for part in arguments.match)]
    return names


def main():
    arguments = parse_args()
    names = select(arguments)
    if arguments.list:
        for name in names:
            print(f"{TEST_GROUPS[name]}\t{name}")
        return
    if not names:
        raise SystemExit(f"no integration tests selected (group={arguments.group})")
    label = arguments.group if not (arguments.test or arguments.match) else "selected"
    with tempfile.TemporaryDirectory(prefix="uagent-integration-") as temp:
        root = pathlib.Path(temp)
        # A temp directory of its own, beside the case homes rather than above
        # them. The runner's own TMPDIR is an ancestor of everything under
        # `root`, including each case's ~/.uagent, so leaving it in place would
        # make the sandbox drop it as a writable root and warn about it in
        # every session the suite starts.
        tmpdir = root / "tmp"
        tmpdir.mkdir()
        os.environ["TMPDIR"] = str(tmpdir)
        for name in names:
            print(f"running {name}", flush=True)
            case_root = root / name
            # HOME is a sibling of the case workspace, not a child of it. A
            # workspace containing ~/.uagent is an ancestor of the agent's own
            # state, which the sandbox refuses to grant as a writable root --
            # so nesting them would leave every case running with nothing it
            # could write, which is not how a real session is laid out.
            home = root / (name + ".home")
            case_root.mkdir(parents=True)
            home.mkdir(parents=True)
            started = time.monotonic()
            ALL_TESTS[name](case_root, home, binary=arguments.binary.resolve())
            print(f"passed {name} ({time.monotonic() - started:.3f}s)", flush=True)
        print(f"all {len(names)} {label} integration tests passed")


if __name__ == "__main__":
    sys.exit(main())
