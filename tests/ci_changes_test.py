"""Keep the lightweight CI path conservative as the repository grows."""

import json
import runpy
import unittest
from pathlib import Path

changes = runpy.run_path(str(Path(__file__).resolve().parents[1] / ".github/changes.py"))
classify, heavy, tidy = changes["classify"], changes["heavy"], changes["tidy"]
build_matrix, tidy_files = changes["build_matrix"], changes["tidy_files"]


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
            "skills/uagent-config/SKILL.md",
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

    def test_clang_tidy_patterns_name_first_party_units_of_the_build(self):
        entries = [
            {"file": f"/r/{path}"}
            for path in ("src/a.cc", "src/b.cc", "tests/unit/c_test.cc", "build/tidy/web_assets.cc")
        ]
        self.assertEqual(tidy_files(entries, "src/b.cc src/gone.cc"), r"/r/src/b\.cc$")
        self.assertEqual(
            tidy_files(entries, "all"), r"/r/src/a\.cc$ /r/src/b\.cc$ /r/tests/unit/c_test\.cc$"
        )
        self.assertEqual(tidy_files(entries, "all", "1/2"), r"/r/src/b\.cc$")

    def test_slow_legs_run_on_a_pull_request_only_where_they_matter(self):
        texts = {
            "src/main.cc": "#if UAGENT_BROWSER\n",
            "src/app/bootstrap.cc": "#ifdef UAGENT_WEB_PUSH\n",
        }

        def read(path):
            return texts.get(path, "int main() {}\n")

        def nothing(_path):
            return ""

        plain = ["src/agent/loop.cc", "tests/unit/stream_test.cc", "web/a.ts"]
        self.assertFalse(heavy(plain, read, read))
        # A guard removed (only the old side has it), or a guarded file
        # renamed (the old path is gone, the new one is all added).
        self.assertTrue(heavy([*plain, "src/main.cc"], read, nothing))
        self.assertTrue(heavy([*plain, "src/app/bootstrap.cc"], nothing, read))
        for path in (
            "CMakeLists.txt",
            ".github/workflows/ci.yml",
            "src/browser/runtime.cc",
            "src/web/no_push.cc",
            "include/web/master.h",
            "src/main.cc",
            "src/app/bootstrap.cc",
        ):
            with self.subTest(path=path):
                self.assertTrue(heavy([*plain, path], read, read))

    def test_arm64_is_in_the_matrix_only_when_asked_for(self):
        def targets(with_arm):
            return [leg["target"] for leg in json.loads(build_matrix(with_arm))["include"]]

        self.assertEqual(targets(False), ["linux-x86_64", "macos-arm64"])
        self.assertEqual(targets(True), ["linux-x86_64", "linux-arm64", "macos-arm64"])


if __name__ == "__main__":
    unittest.main()
