"""Keep the lightweight CI path conservative as the repository grows."""

import runpy
import unittest
from pathlib import Path

changes = runpy.run_path(str(Path(__file__).resolve().parents[1] / ".github/changes.py"))
classify, tidy = changes["classify"], changes["tidy"]


class ChangeSelectionTest(unittest.TestCase):
    def test_frontend_and_docs_keep_their_required_checks(self):
        self.assertEqual(
            classify(["web/src/raw.tsx", "docs/WEB.md"]), {"native": False, "web": True}
        )
        self.assertEqual(
            classify(["docs/TESTING.md", "README.md"]), {"native": False, "web": False}
        )

    def test_shared_code_and_unknown_paths_require_every_suite(self):
        for path in (
            "include/agent/prompt.h",
            "src/removed.cc",
            "CMakeLists.txt",
            "tests/web_host.py",
            ".github/workflows/ci.yml",
            "skills/uagent-config/references/system-prompt.md",
            "new-directory/file",
        ):
            with self.subTest(path=path):
                self.assertEqual(classify(["web/src/raw.tsx", path]), {"native": True, "web": True})

    def test_clang_tidy_reads_changed_units_unless_any_unit_may_be_affected(self):
        self.assertEqual(
            tidy(["src/app/chat.cc", "tests/unit/stream_test.cc", "docs/WEB.md", "web/a.ts"]),
            "src/app/chat.cc tests/unit/stream_test.cc",
        )
        self.assertEqual(tidy(["tests/integration.py"]), "")
        for path in ("include/app/chat.h", ".clang-tidy", "CMakeLists.txt", ".github/changes.py"):
            with self.subTest(path=path):
                self.assertEqual(tidy(["src/app/chat.cc", path]), "all")


if __name__ == "__main__":
    unittest.main()
