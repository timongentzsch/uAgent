"""Wire-kind contract: browser frames stay in sync with their handlers.

The session plane publishes string-kind frames (state, outcome, metadata,
...); the browser's host-data layer (web/src/state/) switches on them, and
the terminal does the same through its own projections. A renamed or added
frame kind on one side without the other is a silent protocol break, so the
contract is enforced here rather than by inspection.

Two planes share the same spelling: DOCUMENTED_INTERNAL kinds (hello,
refresh, submit, ...) are transport/scheduler traffic between the native
router, worker and supervisor and must never gain a browser handler without
an explicit contract change. Everything else published from the
browser-bound session units must be a BROWSER_FRAMES member.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SESSION_UNITS = (
    "session_host.cc",
    "session_router.cc",
    "session_server.cc",
    "session_snapshot.cc",
    "session_supervisor.cc",
    "session_schedules.cc",
    "session_terminal.cc",
    "session_transport.cc",
    "session_worker.cc",
)
WEB_STATE = ("web/src/state", "web/src/app")
# Frame handlers live in the host-data layer; the app shell only issues
# operations (act/command) that share some spellings on another plane.
WEB_HANDLERS = ("web/src/state",)

# Frames the browser consumes: every member needs a native producer in the
# session units and a handler literal under WEB_STATE.
BROWSER_FRAMES = frozenset(
    {
        "activated",
        "activity",
        "closed",
        "deactivated",
        "deleted",
        "error",
        "event",
        "gap",
        "management.changed",
        "metadata",
        "outcome",
        "scheduled.changed",
        "state",
    }
)

# Transport/scheduler-plane traffic with no browser handler. Adding a
# web/src/state handler for one of these (or a new kind entirely) means the
# layering changed: update this contract, not just the code.
DOCUMENTED_INTERNAL = frozenset(
    {
        "close",
        "fork",
        "hello",
        "interrupt",
        "refresh",
        "reply",
        "rewind",
        "share",
        "submit",
    }
)

PRODUCED = re.compile(r'\{"kind",\s*"([A-Za-z._-]+)"')


def produced_kinds():
    kinds = {}
    for unit in SESSION_UNITS:
        path = ROOT / "src/app" / unit
        if not path.exists():
            continue
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            for kind in PRODUCED.findall(line):
                kinds.setdefault(kind, []).append(f"{unit}:{lineno}")
    return kinds


def web_handled_kind(kind):
    needle = f'"{kind}"'
    for tree in WEB_HANDLERS:
        for path in (ROOT / tree).rglob("*"):
            if path.suffix in (".ts", ".tsx") and needle in path.read_text():
                return True
    return False


class WireContractTest(unittest.TestCase):
    def test_browser_frames_have_native_producers(self):
        produced = produced_kinds()
        missing = sorted(k for k in BROWSER_FRAMES if k not in produced)
        self.assertEqual(missing, [])

    def test_browser_frames_have_web_handlers(self):
        missing = sorted(k for k in BROWSER_FRAMES if not web_handled_kind(k))
        self.assertEqual(missing, [])

    def test_internal_kinds_have_no_web_handlers(self):
        leaked = sorted(k for k in DOCUMENTED_INTERNAL if web_handled_kind(k))
        self.assertEqual(leaked, [])

    def test_no_undocumented_session_kinds(self):
        produced = produced_kinds()
        unknown = sorted(k for k in produced if k not in BROWSER_FRAMES | DOCUMENTED_INTERNAL)
        self.assertEqual(
            unknown,
            [],
            "new session frame kinds need a contract entry: browser-handled "
            "goes in BROWSER_FRAMES (with a web/src/state handler), "
            "transport-plane traffic in DOCUMENTED_INTERNAL",
        )


if __name__ == "__main__":
    unittest.main()
