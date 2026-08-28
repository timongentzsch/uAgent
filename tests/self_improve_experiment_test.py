#!/usr/bin/env python3
"""Focused lifecycle tests for the installed self-improvement runner."""

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "skills" / "self-improve" / "scripts" / "experiment.py"


class ExperimentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = pathlib.Path(self.temporary.name)
        self.state = self.base / "state"
        self.candidate = self.base / "candidate.json"
        self.target = self.base / "active.json"
        self.authority = self.base / "authority.json"
        self.candidate.write_text('{"append":"Test narrowly."}\n')
        fixture = ROOT / "tests" / "fixtures" / "eval" / "cheap_authority.json"
        self.authority.write_bytes(fixture.read_bytes())
        self.authority_sha256 = hashlib.sha256(self.authority.read_bytes()).hexdigest()

    def run_command(self, *arguments, ok=True):
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--root", str(self.state), *arguments],
            text=True,
            capture_output=True,
            check=False,
        )
        if ok and result.returncode != 0:
            self.fail(f"command failed: {result.stderr}\n{result.stdout}")
        if not ok and result.returncode == 0:
            self.fail(f"command unexpectedly passed: {result.stdout}")
        return result

    def initialize(
        self,
        *,
        trials="2",
        max_cost="2",
        cost_basis="reported-cost",
        cost_authority=None,
        guardrail_mode="all-trials",
        previous_setting="/previous/overlay.json",
        ok=True,
    ):
        arguments = [
            "init",
            "--id",
            "pilot",
            "--hypothesis",
            "The treatment increases held-out task success.",
            "--overlay",
            str(self.candidate),
            "--target-overlay",
            str(self.target),
            "--model",
            "provider/model",
            "--effort",
            "high",
            "--trials",
            trials,
            "--max-cost",
            max_cost,
            "--cost-basis",
            cost_basis,
            "--guardrail-mode",
            guardrail_mode,
        ]
        if cost_authority is not None:
            arguments.extend(("--cost-authority", str(cost_authority)))
        if previous_setting is not None:
            arguments.extend(("--previous-setting", previous_setting))
        return self.run_command(*arguments, ok=ok)

    def record(
        self,
        variant,
        trial,
        success,
        *,
        cost="0.10",
        tokens="100",
        wall_ms="1000",
        failures="0",
        task_id=None,
        model="provider/model",
        effort="high",
        authority_sha256=None,
        ok=True,
    ):
        arguments = [
            "record",
            "--id",
            "pilot",
            "--variant",
            variant,
            "--trial",
            str(trial),
            "--task-id",
            task_id or f"task-{trial}",
            "--model",
            model,
            "--effort",
            effort,
            "--success",
            success,
            "--cost",
            cost,
            "--tokens",
            tokens,
            "--wall-ms",
            wall_ms,
            "--tool-failures",
            failures,
        ]
        if authority_sha256 is not None:
            arguments.extend(("--authority-sha256", authority_sha256))
        return self.run_command(*arguments, ok=ok)

    def test_non_billable_cheap_requires_explicit_zero_cost(self):
        missing = self.initialize(
            trials="1", max_cost="0", cost_basis="non-billable-cheap", ok=False
        )
        self.assertIn("require --cost-authority", missing.stderr)
        self.initialize(
            trials="1",
            max_cost="0",
            cost_basis="non-billable-cheap",
            cost_authority=self.authority,
        )
        rejected = self.record(
            "control",
            1,
            "yes",
            cost="0.01",
            authority_sha256=self.authority_sha256,
            ok=False,
        )
        self.assertIn("trial cost must be 0", rejected.stderr)
        missing_digest = self.record("control", 1, "no", cost="0", ok=False)
        self.assertIn("authority digest differs", missing_digest.stderr)
        self.record("control", 1, "no", cost="0", authority_sha256=self.authority_sha256)
        self.record("treatment", 1, "yes", cost="0", authority_sha256=self.authority_sha256)
        status = json.loads(self.run_command("status", "--id", "pilot").stdout)
        self.assertEqual(status["cohort"]["cost_basis"], "non-billable-cheap")
        self.assertEqual(status["spent_usd"], 0)

    def test_paired_success_guardrail_branches(self):
        cases = [
            (
                "treatment-gain",
                [("no", "yes", "100", "10000")],
                "pass",
                {"treatment_only": 1, "control_only": 0, "both_pass": 0, "both_fail": 0},
            ),
            (
                "capability-regression",
                [("yes", "no", "100", "100")],
                "reject",
                {"treatment_only": 0, "control_only": 1, "both_pass": 0, "both_fail": 0},
            ),
            (
                "both-pass-above",
                [("no", "yes", "100", "10000"), ("yes", "yes", "1000", "1200")],
                "reject",
                {"treatment_only": 1, "control_only": 0, "both_pass": 1, "both_fail": 0},
            ),
            (
                "both-pass-within",
                [("no", "yes", "100", "10000"), ("yes", "yes", "1000", "1050")],
                "pass",
                {"treatment_only": 1, "control_only": 0, "both_pass": 1, "both_fail": 0},
            ),
            (
                "both-fail",
                [("no", "no", "100", "10000")],
                "inconclusive",
                {"treatment_only": 0, "control_only": 0, "both_pass": 0, "both_fail": 1},
            ),
        ]
        for name, pairs, verdict, expected_pairs in cases:
            with self.subTest(name=name):
                self.state = self.base / f"state-{name}"
                self.initialize(trials=str(len(pairs)), guardrail_mode="paired-success")
                for trial, (control, treatment, control_wall, treatment_wall) in enumerate(
                    pairs, start=1
                ):
                    self.record("control", trial, control, wall_ms=control_wall)
                    self.record("treatment", trial, treatment, wall_ms=treatment_wall)
                review = json.loads(self.run_command("review", "--id", "pilot").stdout)["review"]
                self.assertEqual(review["verdict"], verdict)
                self.assertEqual(review["guardrail_mode"], "paired-success")
                for key, value in expected_pairs.items():
                    self.assertEqual(review["pairs"][key], value)
                self.assertEqual(
                    review["guardrails"]["wall_ms"]["compared_pairs"],
                    expected_pairs["both_pass"],
                )

    def test_passing_lifecycle_restores_exact_snapshot(self):
        previous = b'{"replace":{"Changes":"Original bytes."}}\n'
        self.target.write_bytes(previous)
        self.target.chmod(0o1640)
        self.initialize()
        self.record("control", 1, "yes")
        self.record("control", 2, "no")
        self.record("treatment", 1, "yes")
        self.record("treatment", 2, "yes")

        review = json.loads(self.run_command("review", "--id", "pilot").stdout)
        self.assertEqual(review["review"]["verdict"], "pass")
        self.assertNotIn("guardrail_mode", review["review"])
        self.assertNotIn("pairs", review["review"])
        self.assertNotIn("compared_pairs", review["review"]["guardrails"]["wall_ms"])
        self.assertEqual(review["activation_proposal"]["tool"], "uagent_configure")

        blocked = self.run_command("activate", "--id", "pilot", ok=False)
        self.assertIn("requires --approve", blocked.stderr)
        activated = json.loads(self.run_command("activate", "--id", "pilot", "--approve").stdout)
        self.assertEqual(activated["status"], "overlay_written")
        self.assertEqual(self.target.read_bytes(), self.candidate.read_bytes())
        self.assertIn("did not change", activated["warning"])

        rolled_back = json.loads(self.run_command("rollback", "--id", "pilot").stdout)
        self.assertEqual(self.target.read_bytes(), previous)
        self.assertEqual(self.target.stat().st_mode & 0o7777, 0o1640)
        change = rolled_back["configuration_proposal"]["changes"][0]
        self.assertEqual(change["value"], "/previous/overlay.json")
        status = json.loads(self.run_command("status", "--id", "pilot").stdout)
        self.assertEqual(status["status"], "rolled_back")

    def test_absent_target_is_removed_on_rollback(self):
        self.initialize(trials="1", previous_setting=None)
        self.record("control", 1, "no")
        self.record("treatment", 1, "yes")
        self.run_command("review", "--id", "pilot")
        self.run_command("activate", "--id", "pilot", "--approve")
        self.assertTrue(self.target.exists())
        rolled_back = json.loads(self.run_command("rollback", "--id", "pilot").stdout)
        self.assertFalse(self.target.exists())
        self.assertEqual(rolled_back["configuration_proposal"]["changes"][0]["operation"], "unset")

    def test_rejects_invalid_bounds_cost_and_schema(self):
        zero_reported = self.initialize(max_cost="0", ok=False)
        self.assertIn("reported-cost experiments require a positive max_cost", zero_reported.stderr)

        self.candidate.write_text('{"append":{"wrong":"shape"}}\n')
        invalid_overlay = self.initialize(ok=False)
        self.assertIn("overlay.append must be a nonempty string", invalid_overlay.stderr)
        self.candidate.write_text('{"append":"Test narrowly."}\n')

        invalid = self.initialize(trials="51", ok=False)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("trials must be between 1 and 50", invalid.stderr)

        self.state = self.base / "cost-state"
        self.initialize(trials="1", max_cost="0.1")
        drift = self.record("control", 1, "yes", model="other/model", ok=False)
        self.assertIn("model differs", drift.stderr)
        costly = self.record("control", 1, "yes", cost="0.11", ok=False)
        self.assertNotEqual(costly.returncode, 0)
        self.assertIn("exceeds $0.100000", costly.stderr)

        manifest = self.state / "rounds" / "pilot" / "experiment.json"
        body = json.loads(manifest.read_text())
        body["schema"] = "future.v99"
        manifest.write_text(json.dumps(body))
        bad_schema = self.run_command("status", "--id", "pilot", ok=False)
        self.assertIn("unsupported experiment schema", bad_schema.stderr)

    def test_rejects_symbolic_link_target(self):
        real_target = self.base / "real.json"
        real_target.write_text('{"append":"Existing."}\n')
        self.target.symlink_to(real_target)
        result = self.initialize(ok=False)
        self.assertIn("may not be a symbolic link", result.stderr)
        self.assertFalse((self.state / "rounds" / "pilot").exists())
        self.assertEqual(real_target.read_text(), '{"append":"Existing."}\n')

    def test_review_rejects_mismatched_task_pair(self):
        self.initialize(trials="1")
        self.record("control", 1, "no", task_id="task-a")
        self.record("treatment", 1, "yes", task_id="task-b")
        result = self.run_command("review", "--id", "pilot", ok=False)
        self.assertIn("task_id differ", result.stderr)

    def test_unknown_state_fields_are_rejected(self):
        self.initialize(trials="1")
        results_path = self.state / "rounds" / "pilot" / "results.json"
        results = json.loads(results_path.read_text())
        results["raw_prompt"] = "must never be retained"
        results_path.write_text(json.dumps(results))
        result = self.run_command("status", "--id", "pilot", ok=False)
        self.assertIn("unknown=['raw_prompt']", result.stderr)

    def test_oversized_state_file_is_rejected(self):
        self.initialize(trials="1")
        results_path = self.state / "rounds" / "pilot" / "results.json"
        results_path.write_bytes(b" " * (256 * 1024 + 1))
        result = self.run_command("status", "--id", "pilot", ok=False)
        self.assertIn("state file exceeds 262144 bytes", result.stderr)

    def test_permission_change_blocks_destructive_rollback(self):
        self.initialize(trials="1")
        self.record("control", 1, "no")
        self.record("treatment", 1, "yes")
        self.run_command("review", "--id", "pilot")
        self.run_command("activate", "--id", "pilot", "--approve")
        self.target.chmod(0o640)
        result = self.run_command("rollback", "--id", "pilot", ok=False)
        self.assertIn("mode changed externally", result.stderr)

    def test_external_change_blocks_destructive_rollback(self):
        self.initialize(trials="1")
        self.record("control", 1, "no")
        self.record("treatment", 1, "yes")
        self.run_command("review", "--id", "pilot")
        self.run_command("activate", "--id", "pilot", "--approve")
        self.target.write_text('{"append":"External edit."}\n')
        result = self.run_command("rollback", "--id", "pilot", ok=False)
        self.assertIn("changed externally", result.stderr)


if __name__ == "__main__":
    unittest.main()
