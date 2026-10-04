#!/usr/bin/env python3
import argparse
import importlib
import os
import pathlib
import pkgutil
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import integration_delegation
import integration_management
import integration_mcp
import integration_providers
import integration_runtime
import integration_sandbox
import integration_tools
import integration_ui
import integration_web

TEST_MODULES = (
    ("runtime", integration_runtime),
    ("tools", integration_tools),
    ("ui", integration_ui),
    ("providers", integration_providers),
    ("mcp", integration_mcp),
    ("delegation", integration_delegation),
    ("sandbox", integration_sandbox),
    ("web", integration_web),
    ("management", integration_management),
)


def iter_group_modules(module):
    yield module
    if hasattr(module, "__path__"):
        for info in pkgutil.iter_modules(module.__path__):
            if info.name.startswith("_"):
                continue
            yield importlib.import_module(f"{module.__name__}.{info.name}")


ALL_TESTS = {}
TEST_GROUPS = {}
for _group, _top in TEST_MODULES:
    for _mod in iter_group_modules(_top):
        for _name, _test in vars(_mod).items():
            if (
                _name.startswith("test_")
                and callable(_test)
                and getattr(_test, "__module__", None) == _mod.__name__
            ):
                ALL_TESTS[_name] = _test
                TEST_GROUPS[_name] = _group
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
    parser.add_argument(
        "-j", "--jobs", type=int, default=1, help="run the selection in this many processes"
    )
    arguments = parser.parse_args()
    if arguments.jobs < 1:
        parser.error("--jobs must be at least 1")
    return arguments


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


def remove_suite(root):
    """Remove the suite's directory once no runtime writes into it: a
    session that finished at the end of a case can still start another (a
    thread waking its coordinator) after that case's cleanup."""
    from integration_support import budget
    from session_support import close_sessions, runtime_directory

    homes = list(root.glob("*.home"))
    deadline = time.monotonic() + budget(5)
    quiet_since = time.monotonic()
    # A suite in which no runtime ever started has none to wait for.
    if not any(runtime_directory(home).exists() for home in homes):
        deadline = quiet_since
    while time.monotonic() < deadline and time.monotonic() - quiet_since < 1:
        live = [home for home in homes if any(runtime_directory(home).glob("*.sock"))]
        for home in live:
            close_sessions(home)
        if live:
            quiet_since = time.monotonic()
        time.sleep(0.05)
    for attempt in range(10):
        try:
            shutil.rmtree(root)
            return
        except OSError:
            if attempt == 9:
                raise
            time.sleep(0.5)


def run_shards(arguments, names):
    """The selection dealt round-robin to `jobs` runners of this script, each
    with a suite directory of its own; a case is already isolated by its
    home and its ports. A runner writes to a file, so none waits on another
    for its output to be read; each is shown, with how it ended, when all
    are done."""
    jobs = min(arguments.jobs, len(names))
    runners = []
    try:
        for index in range(jobs):
            output = tempfile.TemporaryFile(mode="w+")
            command = [sys.executable, __file__, str(arguments.binary)]
            for name in names[index::jobs]:
                command += ["--test", name]
            runners.append(
                (subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT), output)
            )
        failed = 0
        for index, (runner, output) in enumerate(runners):
            status = runner.wait()
            output.seek(0)
            sys.stdout.write(output.read())
            if status:
                failed += 1
                print(f"runner {index + 1} of {jobs} failed (exit {status})", flush=True)
    finally:
        # Interrupted: each runner is asked to stop, so it closes its hosts
        # and removes its suite, and killed only if it does not.
        live = [runner for runner, _ in runners if runner.poll() is None]
        for runner in live:
            runner.send_signal(signal.SIGINT)
        for runner in live:
            try:
                runner.wait(timeout=15)
            except subprocess.TimeoutExpired:
                runner.kill()
                runner.wait()
        for _, output in runners:
            output.close()
    if failed:
        raise SystemExit(f"{failed} of {jobs} runners failed")
    print(f"all {len(names)} integration tests passed in {jobs} runners")


def main():
    arguments = parse_args()
    names = select(arguments)
    if arguments.list:
        for name in names:
            print(f"{TEST_GROUPS[name]}\t{name}")
        return
    if not names:
        raise SystemExit(f"no integration tests selected (group={arguments.group})")
    if arguments.jobs > 1 and len(names) > 1:
        return run_shards(arguments, names)
    label = arguments.group if not (arguments.test or arguments.match) else "selected"
    temp = tempfile.mkdtemp(prefix="uagent-integration-")
    try:
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
            try:
                ALL_TESTS[name](case_root, home, binary=arguments.binary.resolve())
            finally:
                from session_support import stop_sessions

                stop_sessions(home)
                for state in case_root.rglob(".uagent"):
                    stop_sessions(state.parent)
            print(f"passed {name} ({time.monotonic() - started:.3f}s)", flush=True)
        print(f"all {len(names)} {label} integration tests passed")
    finally:
        remove_suite(pathlib.Path(temp))


if __name__ == "__main__":
    sys.exit(main())
