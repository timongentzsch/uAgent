#!/usr/bin/env python3
import argparse
import pathlib
import sys
import tempfile

from integration_delegation import TESTS as DELEGATION_TESTS
from integration_mcp import TESTS as MCP_TESTS
from integration_providers import TESTS as PROVIDERS_TESTS
from integration_runtime import TESTS as RUNTIME_TESTS
from integration_support import TEST_ORDER, integration_group
from integration_tools import TESTS as TOOLS_TESTS
from integration_ui import TESTS as UI_TESTS

ALL_TESTS = {
    test.__name__: test
    for test in (
        *RUNTIME_TESTS,
        *TOOLS_TESTS,
        *UI_TESTS,
        *PROVIDERS_TESTS,
        *MCP_TESTS,
        *DELEGATION_TESTS,
    )
}


def check_registration():
    """Execution is driven by TEST_ORDER alone.

    A case added to a module's TESTS but not to TEST_ORDER would never run and
    nothing would say so, which is exactly the failure a regression test is
    supposed to prevent. Refuse to run instead.
    """
    unordered = sorted(set(ALL_TESTS) - set(TEST_ORDER))
    undefined = sorted(set(TEST_ORDER) - set(ALL_TESTS))
    problems = []
    if unordered:
        problems.append(f"defined but missing from TEST_ORDER (would never run): {unordered}")
    if undefined:
        problems.append(f"listed in TEST_ORDER but not defined: {undefined}")
    if problems:
        raise SystemExit("integration registration is inconsistent:\n  " + "\n  ".join(problems))


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
        for name in TEST_ORDER
        if arguments.group == "all" or integration_group(name) == arguments.group
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
    check_registration()
    arguments = parse_args()
    names = select(arguments)
    if arguments.list:
        for name in names:
            print(f"{integration_group(name)}\t{name}")
        return
    if not names:
        raise SystemExit(f"no integration tests selected (group={arguments.group})")
    label = arguments.group if not (arguments.test or arguments.match) else "selected"
    with tempfile.TemporaryDirectory(prefix="uagent-integration-") as temp:
        root = pathlib.Path(temp)
        for name in names:
            print(f"running {name}", flush=True)
            case_root = root / name
            home = case_root / "home"
            home.mkdir(parents=True)
            ALL_TESTS[name](case_root, home)
        print(f"all {len(names)} {label} integration tests passed")


if __name__ == "__main__":
    sys.exit(main())
