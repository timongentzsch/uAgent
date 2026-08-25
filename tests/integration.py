#!/usr/bin/env python3
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


def main():
    requested_group = "all"
    if len(sys.argv) == 4 and sys.argv[2] == "--group":
        requested_group = sys.argv[3]
    elif len(sys.argv) != 2:
        raise SystemExit("usage: integration.py BINARY [--group GROUP]")
    with tempfile.TemporaryDirectory(prefix="uagent-integration-") as temp:
        root = pathlib.Path(temp)
        tests = [
            ALL_TESTS[name]
            for name in TEST_ORDER
            if requested_group == "all" or integration_group(name) == requested_group
        ]
        if not tests:
            raise SystemExit(f"unknown or empty integration group: {requested_group}")
        for test in tests:
            print(f"running {test.__name__}", flush=True)
            case_root = root / test.__name__
            home = case_root / "home"
            home.mkdir(parents=True)
            test(case_root, home)
        print(f"all {len(tests)} {requested_group} integration tests passed")


if __name__ == "__main__":
    main()
