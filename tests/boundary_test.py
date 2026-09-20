"""Consumer-boundary guard: domain layers never include presentation.

The agent, provider-dialect and tool layers emit typed events and data views;
terminal, browser and headless runners consume them. A domain translation unit
that includes ui/ or web/ can steer control flow from presentation code, so the
boundary is enforced here rather than by inspection. Tool-call observation
records live in agent/tool_presentation.h precisely so the loop can attach
them without reaching into ui/.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOMAIN = ("src/agent", "src/api", "src/tools")
DOMAIN_HEADERS = ("include/agent", "include/api", "include/tools")
CONSUMER = re.compile(r'#include\s+"include/(ui|web)/[^"]+"')


def includes_of(path):
    found = []
    for line in path.read_text().splitlines():
        match = CONSUMER.search(line)
        if match:
            found.append(match.group(0))
    return found


class ConsumerBoundaryTest(unittest.TestCase):
    def test_domain_headers_include_no_presentation(self):
        violations = []
        for tree in DOMAIN_HEADERS:
            for path in sorted((ROOT / tree).rglob("*.h")):
                for line in includes_of(path):
                    violations.append(f"{path.relative_to(ROOT)}: {line}")
        self.assertEqual(violations, [])

    def test_domain_sources_include_no_presentation(self):
        violations = []
        for tree in DOMAIN:
            for suffix in ("*.cc", "*.h"):
                for path in sorted((ROOT / tree).rglob(suffix)):
                    rel = path.relative_to(ROOT).as_posix()
                    for line in includes_of(path):
                        violations.append(f"{rel}: {line}")
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
