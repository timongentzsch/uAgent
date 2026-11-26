"""Egress guard: the product names no outside host that SECURITY.md's
"What leaves the machine" table does not list. A new contact has to be
written down there, with when it happens and what it carries."""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCANNED = ["src", "include", "web/src", "web/sheet", "web/public", "web/index.html"]
SCANNED += ["install.sh", "deploy", "skills"]
TEXT = (".cc", ".h", ".ts", ".tsx", ".js", ".css", ".html", ".sh", ".md", ".json", ".yaml")
TEXT += ("Dockerfile",)
URL = re.compile(r"https?://((?:[a-z0-9-]+\.)+[a-z]{2,})", re.I)
# A host written as a string of its own: an allow-list entry, a table key.
HOST = r"(?:[a-z0-9-]+\.)+(?:com|org|net|ai|io|dev)"
QUOTED = re.compile(rf"""["'`]({HOST})["'`/:]""")
# Named, never contacted by the product: what each is there for.
MENTIONED = {
    "www.w3.org": "XML namespaces",
    "json-schema.org": "the settings schema's id",
    "www.apache.org": "a licence text",
    "www.apple.com": "the launchd property list's DTD",
    "clang.llvm.org": "a comment's reference",
    "html.spec.whatwg.org": "a comment's reference",
    "agentskills.io": "a link in the skills README",
    "api.groq.com": "an example provider in the configuration skill",
    "example.com": "what deploy/check-network.sh proves unreachable",
    "passwords.google.com": "blocked in Chrome's policy",
    "docs.docker.com": "the installer's link to Docker's install guide",
}


def named_hosts():
    hosts = {}
    listed = subprocess.check_output(["git", "ls-files", *SCANNED], cwd=ROOT, text=True)
    for path in listed.splitlines():
        if not path.endswith(TEXT):
            continue
        text = (ROOT / path).read_text(errors="replace")
        for host in URL.findall(text) + QUOTED.findall(text):
            hosts.setdefault(host.lower(), path)
    return hosts


def listed_hosts():
    page = (ROOT / "SECURITY.md").read_text()
    table = page.split("## What leaves the machine", 1)[1].split("\n## ", 1)[0]
    return set(re.findall(rf"`({HOST})`", table))


class EgressTest(unittest.TestCase):
    def test_every_named_host_is_listed(self):
        listed = listed_hosts()
        unlisted = {
            host: path
            for host, path in named_hosts().items()
            if host not in listed and host not in MENTIONED
        }
        self.assertEqual(unlisted, {}, "add each to SECURITY.md's table")

    def test_no_listed_host_is_stale(self):
        self.assertEqual(listed_hosts() - set(named_hosts()), set())


if __name__ == "__main__":
    unittest.main()
