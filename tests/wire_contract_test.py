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
    "src/app/session_host.cc",
    "src/app/session_router.cc",
    "src/app/session_server.cc",
    "src/app/session_snapshot.cc",
    "src/app/session_supervisor.cc",
    "src/app/session_schedules.cc",
    "src/app/session_terminal.cc",
    "src/app/session_transport.cc",
    "src/app/session_worker.cc",
    # The view reducer builds the `block` patches the host publishes.
    "src/agent/view_reducer.cc",
)
WEB_STATE = ("web/src/state", "web/src/app")
# Frame handlers live in the host-data layer; the app shell only issues
# operations (act/command) that share some spellings on another plane.
WEB_HANDLERS = ("web/src/state",)

# Web-side types that never cross the wire, and field prefixes the host
# builds at runtime (PrefixNumericStatistics).
WEB_ONLY_TYPES = {"PresentedBlock", "RawOptions"}
BUILT_PREFIXES = ("side_",)

# Frames the browser consumes: every member needs a native producer in the
# session units and a handler literal under WEB_STATE.
BROWSER_FRAMES = frozenset(
    {
        "activated",
        "block",
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
        "spaces.changed",
        "state",
        "turn_status",
    }
)

# Transport/scheduler-plane traffic with no browser handler. Adding a
# web/src/state handler for one of these (or a new kind entirely) means the
# layering changed: update this contract, not just the code.
DOCUMENTED_INTERNAL = frozenset(
    {
        "close",
        # A panel yields a thread's routed decision to the user.
        "escalate",
        "fork",
        "hello",
        "interrupt",
        "refresh",
        "reply",
        # A runtime with nothing to do asks its watchers to let go.
        "retiring",
        "share",
        "side",
        # A thread's finished turn, sent to its panel's runtime.
        "steer",
        "submit",
    }
)

# Spelled like a frame kind but never one: `activity` is the terminal's
# command for activity operations.
ROW_KINDS = frozenset({"activity"})

PRODUCED = re.compile(r'\{"kind",\s*"([A-Za-z._-]+)"')


def produced_kinds():
    kinds = {}
    for unit in SESSION_UNITS:
        path = ROOT / unit
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
    def test_web_types_name_fields_the_host_sends(self):
        # A field the web declares for host data but no native source spells
        # is dead or drifted (a rename on one side only).
        types = (ROOT / "web/src/shared/types.ts").read_text(encoding="utf-8")
        native = "".join(
            path.read_text(encoding="utf-8")
            for folder in ("src", "include")
            for path in (ROOT / folder).rglob("*.[ch]*")
        )
        missing = []
        for match in re.finditer(
            r"export interface (\w+)(?: extends [^{]+)? \{(.*?)\n\}", types, re.S
        ):
            name, body = match.groups()
            if name in WEB_ONLY_TYPES:
                continue
            for field in re.findall(r"^\s{2}([a-z_][a-z0-9_]*)\??:", body, re.M):
                spelled = f'"{field}"' in native or any(
                    field.startswith(prefix) and f'"{prefix}"' in native
                    for prefix in BUILT_PREFIXES
                )
                if not spelled:
                    missing.append(f"{name}.{field}")
        self.assertEqual(missing, [])

    def test_nodes_carry_no_private_paths(self):
        # Every node goes to browsers; a path into the private store does
        # not. The host keeps a diff's path itself (change_paths) and reads
        # an exchange from the saved conversation. A builder may ask whether
        # a record has one, never copy it into a node.
        written = re.compile(r'[{\[,]\s*"(change_path|exchange_path)"')
        builders = (
            "include/agent/node.h",
            "src/agent/transcript_tree.cc",
            "src/agent/view_reducer.cc",
        )
        for unit in builders:
            text = (ROOT / unit).read_text(encoding="utf-8")
            self.assertEqual(written.findall(text), [], unit)

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
        unknown = sorted(
            k for k in produced if k not in BROWSER_FRAMES | DOCUMENTED_INTERNAL | ROW_KINDS
        )
        self.assertEqual(
            unknown,
            [],
            "new session frame kinds need a contract entry: browser-handled "
            "goes in BROWSER_FRAMES (with a web/src/state handler), "
            "transport-plane traffic in DOCUMENTED_INTERNAL",
        )

    def test_terminal_socket_messages_match_the_host(self):
        # A terminal socket's text frames: typed in the web's unions, built
        # and read by the host's relay. A type on one side only is ignored
        # on the other without a word.
        types = (ROOT / "web/src/shared/types.ts").read_text(encoding="utf-8")
        relay = (ROOT / "src/web/master_terminal.cc").read_text(encoding="utf-8")
        web = set()
        for name in ("TerminalControl", "TerminalNotice"):
            union = re.search(rf"export type {name} =(.*?);\n", types, re.S)
            self.assertIsNotNone(union, name)
            web |= set(re.findall(r'type: "([a-z_]+)"', union.group(1)))
        native = set(re.findall(r'\{"type", "([a-z_]+)"\}', relay))
        native |= set(re.findall(r'"type", ""\) != "([a-z_]+)"', relay))
        self.assertEqual(web, native)
        self.assertEqual(web, {"exit", "replaced", "resize"})


if __name__ == "__main__":
    unittest.main()
