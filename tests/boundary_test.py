"""Consumer-boundary guard: domain layers never include presentation.

The agent, provider-dialect and tool layers emit typed events and data views;
terminal, browser and headless runners consume them. A domain translation unit
that includes ui/ or web/ can steer control flow from presentation code, so the
boundary is enforced here rather than by inspection. The single exemption is
tool_loop.cc's use of the state-free presentation builders in
ui/tool_output.h; moving those is a module-specific extraction, not a
mechanical relocation.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOMAIN = ("src/agent", "src/api", "src/tools")
DOMAIN_HEADERS = ("include/agent", "include/api", "include/tools")
CONSUMER = re.compile(r'#include\s+"include/(ui|web)/[^"]+"')

# (relative path, included header): pure state-free builders, deferred.
ALLOWED = {
    ("src/agent/tool_loop.cc", "include/ui/tool_output.h"),
}


def includes_of(path):
    found = []
    for line in path.read_text().splitlines():
        match = CONSUMER.search(line)
        if match:
            found.append(match.group(0))
    return found


def header_name(include_line):
    return include_line.split('"')[1]


class ConsumerBoundaryTest(unittest.TestCase):
    def test_domain_headers_include_no_presentation(self):
        violations = []
        for tree in DOMAIN_HEADERS:
            for path in sorted((ROOT / tree).rglob("*.h")):
                for line in includes_of(path):
                    violations.append(f"{path.relative_to(ROOT)}: {line}")
        self.assertEqual(violations, [])

    def test_domain_sources_include_no_presentation_besides_allowlist(self):
        violations = []
        for tree in DOMAIN:
            for suffix in ("*.cc", "*.h"):
                for path in sorted((ROOT / tree).rglob(suffix)):
                    rel = path.relative_to(ROOT).as_posix()
                    for line in includes_of(path):
                        if (rel, header_name(line)) not in ALLOWED:
                            violations.append(f"{rel}: {line}")
        self.assertEqual(violations, [])

    def test_allowlist_entries_still_exist(self):
        for rel, header in ALLOWED:
            with self.subTest(path=rel):
                self.assertIn(header, Path(ROOT / rel).read_text())


if __name__ == "__main__":
    unittest.main()
