"""Layer guard: a translation unit includes only its own layer or lower ones.

CMake links the libraries as core <- api <- toolcore <- agent <- tools <- app
<- web. Static-library ordering hides an upward include until the symbol it
names moves, so the order is checked here. KNOWN lists the remaining upward
edges; removing one is progress, adding one fails this test.
"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ORDER = ["core", "api", "toolcore", "agent", "tools", "app", "web"]
INCLUDE = re.compile(r'#include\s+"(include/[^"]+)"')

KNOWN = {
    "include/api/stream.h -> include/agent/tool_protocol.h",
    "include/media/attachments.h -> include/api/capabilities.h",
    "include/media/attachments.h -> include/tools/tool.h",
    "include/tools/adapt_system.h -> include/app/prompt_control.h",
    "include/tools/collaborator_runtime.h -> include/app/options.h",
    "include/tools/configure.h -> include/app/config_proposal.h",
    "include/tools/configure.h -> include/app/self_description.h",
    "src/agent/child_agent.cc -> include/tools/session.h",
    "src/agent/request.cc -> include/app/prompt_control.h",
    "src/browser/browser.cc -> include/app/session.h",
    "src/browser/runtime.cc -> include/app/session.h",
    "src/core/events.cc -> include/ui/presentation.h",
    "src/tools/browser.cc -> include/app/session.h",
    "src/tools/browser.cc -> include/cli.h",
    "src/tools/collaborator_runtime.cc -> include/app/session.h",
    "src/tools/session.cc -> include/app/session.h",
}


def layer(path):
    if path.startswith(("src/core/", "src/transport/", "src/media/")):
        return "core"
    if path.startswith(("include/core/", "include/transport/", "include/media/")):
        return "core"
    if path.startswith(("src/api/", "include/api/")) or path == "include/api.h":
        return "api"
    if path in ("src/tools/tool.cc", "include/tools/tool.h", "include/providers.h"):
        return "toolcore"
    if path.startswith("src/providers/"):
        return "toolcore"
    if path.startswith(("src/agent/", "include/agent/")) or path == "include/agent.h":
        return "agent"
    if path.startswith(("src/tools/", "src/mcp/", "src/browser/")):
        return "tools"
    if path.startswith(("include/tools/", "include/mcp/", "include/browser/")):
        return "tools"
    if path.startswith(("src/app/", "src/cli/", "src/ui/", "include/app/", "include/ui/")):
        return "app"
    if path in ("include/cli.h", "include/md.h"):
        return "app"
    if path.startswith(("src/web/", "include/web/")):
        return "web"
    return None


def upward_edges():
    tracked = subprocess.run(
        ["git", "ls-files", "src", "include"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    edges = set()
    for name in tracked:
        own = layer(name)
        if own is None:
            continue
        for target in INCLUDE.findall((ROOT / name).read_text()):
            other = layer(target)
            if other and ORDER.index(other) > ORDER.index(own):
                edges.add(f"{name} -> {target}")
    return edges


class LayerBoundaryTest(unittest.TestCase):
    def test_no_new_upward_includes(self):
        edges = upward_edges()
        self.assertEqual(sorted(edges - KNOWN), [], "new upward include")
        self.assertEqual(sorted(KNOWN - edges), [], "remove fixed edges from KNOWN")


if __name__ == "__main__":
    unittest.main()
