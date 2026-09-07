#!/usr/bin/env python3
"""Hermetic tests for the recursive self-improvement controller.

No model is called. A scripted stand-in binary carries its behaviour inside its
own bundle, so a control and a candidate differ only in the version that was
launched, and every controller invariant — source isolation, exact binary
selection, identical cohort inputs, external budget enforcement, gate
rejection, verdict branches, promotion and rollback — is exercised end to end.
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
SANDBOX = (
    pathlib.Path(sys.argv.pop(1)).resolve()
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-")
    else ROOT / "build/debug/uagent"
)
RUNNER = ROOT / "skills" / "self-improve" / "scripts" / "experiment.py"
FIXTURE = ROOT / "tests" / "fixtures" / "self_improve" / "source"
sys.path.insert(0, str(ROOT / "skills" / "self-improve" / "scripts"))

from agent_run import run_process, tree_digests  # noqa: E402
from experiment import ControllerError, compare, decide, run_gate  # noqa: E402
from integration_support import Server, base_env, event  # noqa: E402

ROUTE = "fixture/model"
VERIFY = "python3 tests/gate_test.py"
IMPROVEMENT = "VALUE = 2\n"


def plan(
    *,
    writes=None,
    deletes=None,
    claim=True,
    cost=0.02,
    sleep=0.0,
    model_requests=2,
    tool_calls=2,
    tool_failures=0,
    exit_code=0,
    verify="measure-increment",
):
    body = {
        "writes": writes or {},
        "deletes": deletes or [],
        "usage": {"input": 400, "output": 100, "cost": cost, "cost_reported": True},
        "model_requests": model_requests,
        "tool_calls": tool_calls,
        "tool_failures": tool_failures,
        "sleep_seconds": sleep,
        "exit": exit_code,
    }
    if claim:
        body["claim"] = {
            "hypothesis": "The improved version reaches the same result for less.",
            "measurement": "The subject gate still passes with the new module present.",
            "verify_command": verify,
            "assessment": {
                "change_summary": "Add an increment to the fixture module.",
                "impact": "Increment check changes from exit 1 to exit 0.",
                "generality": "Only this scripted fixture is exercised.",
                "limitations": "No live model or held-out workloads tested.",
                "recommendation": "propose",
                "proposed_title": "Improve fixture increment",
            },
        }
    return body


def improvement_plan(successor, **overrides):
    """A plan that adds one real module and installs its successor's plan."""
    return plan(
        writes={
            "src/improvement.py": IMPROVEMENT,
            "skills/plan.json": json.dumps(successor, sort_keys=True),
        },
        **overrides,
    )


class ControllerTest(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.base = pathlib.Path(__import__("tempfile").mkdtemp(prefix="uagent-improve-test-"))
        self.addCleanup(shutil.rmtree, self.base, ignore_errors=True)
        self.state = self.base / "state"
        self.source = self.base / "source"
        shutil.copytree(FIXTURE, self.source)
        self.incumbent = self.base / "incumbent"
        (self.incumbent / "skills").mkdir(parents=True)
        binary = self.incumbent / "uagent"
        shutil.copyfile(self.source / "src" / "agent.py", binary)
        binary.chmod(0o755)
        self.binary = binary
        self.authority = self.base / "authority.json"
        self.authority.write_text(
            json.dumps(
                {
                    "schema": "uagent.eval.cost-authority.v1",
                    "routes": {ROUTE: {"reports_cost": True, "enforces_hard_budget": True}},
                }
            ),
            encoding="utf-8",
        )

    # --- helpers ---

    def install_plan(self, body):
        (self.incumbent / "skills" / "plan.json").write_text(
            json.dumps(body, sort_keys=True), encoding="utf-8"
        )

    def run_command(self, *arguments, ok=True):
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--root", str(self.state), *arguments],
            text=True,
            capture_output=True,
            check=False,
        )
        if ok and result.returncode != 0:
            self.fail(f"command failed: {result.stdout}\n{result.stderr}")
        if not ok and result.returncode == 0:
            self.fail(f"command unexpectedly succeeded: {result.stdout}")
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError:
            self.fail(f"command produced no JSON: {result.stdout}\n{result.stderr}")

    def initialize(self, *extra, max_cost="1.00", ok=True):
        return self.run_command(
            "init",
            "--sandbox-binary",
            str(SANDBOX),
            "--source",
            str(self.source),
            "--binary",
            str(self.binary),
            "--skills",
            str(self.incumbent / "skills"),
            "--route",
            ROUTE,
            "--cost-authority",
            str(self.authority),
            "--max-cost",
            max_cost,
            "--artifact",
            "build/uagent",
            "--build-command",
            "python3 build.py",
            "--gate",
            VERIFY,
            "--max-wall-seconds",
            "60",
            "--gate-seconds",
            "60",
            *extra,
            ok=ok,
        )

    def generation_dir(self, number=1):
        return self.state / "generations" / f"{number:04d}"

    def read_state(self):
        return json.loads((self.state / "state.json").read_text(encoding="utf-8"))

    # --- end-to-end ---

    @unittest.skipUnless(sys.platform == "darwin", "Darwin process-group semantics")
    def test_cleanup_accepts_an_exited_group_reporting_eperm(self):
        with patch("agent_run.os.killpg", side_effect=PermissionError("group exited")):
            result = run_process(
                ["/usr/bin/true"],
                workspace=self.source,
                env=os.environ.copy(),
                timeout=5,
                sandbox_binary=SANDBOX,
                writable_roots=(self.source,),
            )
        self.assertEqual(result["returncode"], 0)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin process-group semantics")
    def test_cleanup_rejects_eperm_with_a_live_descendant(self):
        signal_group = os.killpg
        denied = False

        def deny_once(group, number):
            nonlocal denied
            if not denied:
                denied = True
                raise PermissionError("live group")
            signal_group(group, number)

        with patch("agent_run.os.killpg", side_effect=deny_once):
            with self.assertRaisesRegex(PermissionError, "live group"):
                run_process(
                    ["/bin/sh", "-c", "sleep 30 &"],
                    workspace=self.source,
                    env=os.environ.copy(),
                    timeout=5,
                    sandbox_binary=SANDBOX,
                    writable_roots=(self.source,),
                    monitor=lambda: "stop",
                )

    def test_native_executor_can_start_inside_the_outer_sandbox(self):
        home = self.base / "home"
        home.mkdir()
        with Server([event({"content": "ok"})]) as server:
            env = base_env(home, server.url)
            env["UAGENT_TRUST_PROJECT_CONFIG"] = "0"
            result = run_process(
                [str(SANDBOX), "--json", "-p", "Reply ok"],
                workspace=self.source,
                env=env,
                timeout=30,
                sandbox_binary=SANDBOX,
                writable_roots=(self.source, home),
            )
            self.assertEqual(result["returncode"], 0, result)
            self.assertEqual(json.loads(result["stdout"])["answer"], "ok")
            self.assertEqual(len(server.requests), 1)

    def test_promotes_a_cheaper_candidate_then_rolls_back(self):
        successor = improvement_plan(plan(), cost=0.01, sleep=0.0)
        self.install_plan(improvement_plan(successor, cost=0.02, sleep=1.0))
        before = tree_digests(self.source)

        self.initialize()
        discovery = self.run_command("discover")
        self.assertEqual(discovery["run"]["returncode"], 0)
        self.assertGreater(discovery["run"]["cost_usd"], 0)

        gate = self.run_command("gate")
        self.assertIn("src/improvement.py", gate["changed"])
        self.assertIn("src/improvement.py", gate["claim"]["verify_command"])
        early_review = self.run_command("review")

        replay = self.run_command("replay", "--pairs", "1", "--keep-workspaces")
        self.assertEqual(replay["summary"]["control"]["successes"], 1)
        self.assertEqual(replay["summary"]["candidate"]["successes"], 1)
        self.assertEqual(replay["summary"]["verdict"], "promote")

        self.run_command("continue", "--pairs", "1")
        verdict = self.run_command("verdict")
        self.assertEqual(verdict["verdict"], "promote")
        self.assertIsNotNone(verdict["continuation"])

        self.assertIn("review", self.run_command("promote", "--approve", ok=False)["error"])
        self.assertIn(
            "stale",
            self.run_command(
                "promote", "--approve", "--review-id", early_review["review_id"], ok=False
            )["error"],
        )
        review = self.run_command("review")
        self.assertIn(
            "review",
            self.run_command("promote", "--approve", "--review-id", "wrong", ok=False)["error"],
        )
        patch_path = pathlib.Path(review["patch"])
        patch_path.write_text(patch_path.read_text() + "modified after presentation\n")
        self.assertIn(
            "stale",
            self.run_command("promote", "--approve", "--review-id", review["review_id"], ok=False)[
                "error"
            ],
        )
        review = self.run_command("review")
        promotion = self.run_command("promote", "--approve", "--review-id", review["review_id"])
        state = self.read_state()
        self.assertEqual(state["active"]["generation"], 1)
        self.assertEqual(state["active"]["executor"]["identity"], promotion["executor"])
        self.assertEqual(state["previous"]["generation"], 0)

        rolled_back = self.run_command("rollback")
        self.assertEqual(rolled_back["active"]["generation"], 0)
        self.assertEqual(
            self.read_state()["active"]["executor"]["path"],
            str(self.read_state()["active"]["executor"]["path"]),
        )
        # The user's own checkout is never the workspace of an experiment.
        self.assertEqual(tree_digests(self.source), before)

    def test_preflight_failure_spends_no_model_calls(self):
        self.install_plan(improvement_plan(plan()))
        (self.source / "broken.marker").write_text("baseline already broken")
        self.initialize()
        result = self.run_command("discover")
        self.assertEqual(result["status"], "preflight_failed")
        generation = self.generation_dir()
        self.assertFalse((generation / "discovery.json").exists())
        self.assertFalse((generation / "verdict.json").exists())
        self.assertFalse((generation / "work").exists())
        self.assertFalse(json.loads((generation / "preflight.json").read_text())["passed"])

    def test_review_exposes_evidence_and_applicable_patch_without_activation(self):
        (self.source / "src/non_utf8.txt").write_bytes(b"old \xff\n")
        (self.source / "src/binary.dat").write_bytes(b"old\x00bytes")
        (self.source / "src/removed.py").write_text("old = True\n")
        body = improvement_plan(plan())
        body["writes"].update({"src/non_utf8.txt": "new\n", "src/binary.dat": "new\x00bytes"})
        body["deletes"] = ["src/removed.py"]
        self.install_plan(body)
        self.initialize()
        before = self.read_state()
        self.run_command("discover")
        self.assertEqual(self.run_command("gate")["next"], "review")
        review = self.run_command("review")
        packet = json.loads(pathlib.Path(review["evidence"]).read_text())
        self.assertTrue(packet["source_validated"])
        self.assertIsNone(packet["controller_evidence"]["verdict"])
        self.assertIn("held-out", packet["agent_assessment"]["limitations"])
        self.assertIn("Pending human", packet["authorization"])
        report = pathlib.Path(review["report"]).read_text()
        self.assertIn("original (exit 1)", report)
        self.assertIn("Agent assessment: generality", report)
        self.assertEqual(self.run_command("review")["review_id"], review["review_id"])
        applied = subprocess.run(
            ["git", "apply", review["patch"]], cwd=self.source, capture_output=True
        )
        self.assertEqual(applied.returncode, 0, applied.stderr)
        self.assertEqual((self.source / "src/improvement.py").read_text(), IMPROVEMENT)
        self.assertEqual((self.source / "src/non_utf8.txt").read_bytes(), b"new\n")
        self.assertEqual((self.source / "src/binary.dat").read_bytes(), b"new\x00bytes")
        self.assertFalse((self.source / "src/removed.py").exists())
        self.assertEqual(self.read_state(), before)

    def test_review_requires_assessment_even_for_validated_source(self):
        body = improvement_plan(plan())
        del body["claim"]["assessment"]
        self.install_plan(body)
        self.initialize()
        self.run_command("discover")
        self.assertTrue(self.run_command("gate")["source_validated"])
        self.assertIn("complete assessment", self.run_command("review", ok=False)["error"])

    def test_pair_receives_identical_inputs_from_the_recorded_bundles(self):
        successor = improvement_plan(plan(), cost=0.01)
        self.install_plan(improvement_plan(successor, cost=0.02))
        self.initialize()
        self.run_command("discover")
        gate = self.run_command("gate")
        self.run_command("replay", "--pairs", "1", "--keep-workspaces")

        sessions = {}
        for variant in ("control", "candidate"):
            home = self.generation_dir() / "work" / f"replay-1-{variant}" / "home"
            sessions[variant] = json.loads((home / "session.json").read_text(encoding="utf-8"))
            sessions[f"{variant}-prompt"] = (home / "prompt.sha256").read_text(encoding="utf-8")

        # Same instruction, same route, same externally imposed limits.
        self.assertEqual(sessions["control-prompt"], sessions["candidate-prompt"])
        self.assertEqual(sessions["control"]["model"], sessions["candidate"]["model"])
        self.assertEqual(sessions["control"]["limits"], sessions["candidate"]["limits"])
        # Different, explicitly recorded executables: no fallback to one binary.
        bundles = pathlib.Path(os.path.realpath(self.state / "bundles"))
        self.assertNotEqual(sessions["control"]["executable"], sessions["candidate"]["executable"])
        self.assertEqual(
            sessions["candidate"]["executable"],
            str(bundles / gate["candidate_bundle"] / "bin" / "uagent"),
        )
        self.assertTrue(sessions["control"]["executable"].startswith(str(bundles)))
        # Each side started from a byte-identical fresh copy of the subject.
        manifest = json.loads((self.generation_dir() / "manifest.json").read_text(encoding="utf-8"))
        for variant in ("control", "candidate"):
            workspace = self.generation_dir() / "work" / f"replay-1-{variant}" / "source"
            digests = tree_digests(workspace)
            for name, digest in manifest["subject"]["digests"].items():
                if name not in ("src/improvement.py", "skills/plan.json"):
                    self.assertEqual(digests.get(name), digest, name)

    def test_metrics_come_from_the_trace_not_from_the_candidate(self):
        self.install_plan(
            improvement_plan(plan(), cost=0.03, model_requests=2, tool_calls=3, tool_failures=1)
        )
        self.initialize()
        run = self.run_command("discover")["run"]
        self.assertEqual(run["model_requests"], 2)
        self.assertEqual(run["tool_calls"], 3)
        self.assertEqual(run["tool_failures"], 1)
        self.assertEqual(run["tokens"], 500)
        self.assertEqual(run["cost_usd"], 0.03)

    def test_host_verification_runs_native_sandbox_gates_in_clean_copies(self):
        # These gates must be able to exercise the native sandbox themselves.
        # They also reject discovery's stale build output and ancestor path.
        gate = self.source / "tests" / "native_gate.py"
        gate.write_text(
            "import pathlib, subprocess, sys\n"
            f"assert not pathlib.Path.cwd().is_relative_to({str(self.base)!r})\n"
            "assert not pathlib.Path('build/discovery-only').exists()\n"
            "assert not pathlib.Path.home().is_relative_to(pathlib.Path.cwd())\n"
            f"subprocess.run([{str(SANDBOX)!r}, '--sandbox-child', 'net=1', 'roots=1', "
            "str(pathlib.Path.cwd()), '--', sys.executable, '-c', "
            "\"from pathlib import Path; Path('build/nested-ok').write_text('ok')\"], check=True)\n"
        )
        self.install_plan(improvement_plan(improvement_plan(plan())))
        initialized = self.initialize(
            "--gate-mode", "host", "--gate", "python3 tests/native_gate.py"
        )
        self.run_command("discover")
        discovery = self.generation_dir() / "work/discovery/source"
        (discovery / "build").mkdir()
        (discovery / "build/discovery-only").write_text("stale")
        before = tree_digests(discovery)
        gate_result = self.run_command("gate")
        self.assertTrue(gate_result["source_validated"])
        evaluation = json.loads((self.generation_dir() / "gate.json").read_text())["evaluation"]
        self.assertTrue(evaluation["outcome"])
        self.assertTrue(all(gate["mode"] == "host" for gate in evaluation["gates"]))
        self.assertEqual(evaluation["gates"][-1]["returncode"], 1)
        self.assertEqual(tree_digests(discovery), before)
        self.run_command("replay")
        self.run_command("continue")
        self.assertEqual(self.run_command("status")["generations"][0]["status"], "continued")
        self.run_command("verdict")
        self.assertTrue(self.run_command("status")["generations"][0]["source_validated"])
        manifest = json.loads((pathlib.Path(initialized["path"]) / "manifest.json").read_text())
        self.assertEqual(manifest["gates"]["mode"], "host")

    def test_host_gates_still_reject_a_broken_candidate(self):
        self.install_plan(plan(writes={"src/improvement.py": IMPROVEMENT, "broken.marker": "x\n"}))
        self.assert_rejected("--gate-mode", "host", contains="gate failed")

    def test_host_gates_still_reject_an_always_passing_claim(self):
        self.install_plan(improvement_plan(plan(), verify="python3 -c 'pass'"))
        self.assert_rejected("--gate-mode", "host", contains="fail on the original")

    def test_host_verification_does_not_disable_executor_sandbox(self):
        target = self.state / "escaped.txt"
        self.install_plan(plan(writes={str(target): "escaped"}))
        self.initialize("--gate-mode", "host")
        run = self.run_command("discover")["run"]
        self.assertNotEqual(run["returncode"], 0)
        self.assertFalse(target.exists())

    def test_unknown_gate_mode_is_refused(self):
        with self.assertRaisesRegex(ControllerError, "unknown gate mode"):
            run_gate(
                "python3 -c 'pass'",
                self.source,
                self.base / "gate.log",
                60,
                {"gates": {"mode": "typo"}},
            )

    # --- gate rejections ---

    def assert_rejected(self, *init_arguments, contains):
        self.initialize(*init_arguments)
        self.run_command("discover")
        verdict = self.run_command("gate")
        self.assertEqual(verdict["verdict"], "reject")
        self.assertTrue(
            any(contains in reason for reason in verdict["reasons"]),
            f"{contains!r} not in {verdict['reasons']}",
        )
        # A rejected generation never advances the pointer.
        self.assertEqual(self.read_state()["active"]["generation"], 0)
        self.run_command("replay", ok=False)
        return verdict

    def test_rejects_a_candidate_that_edits_a_protected_path(self):
        self.install_plan(plan(writes={"src/agent.py": "# rewritten by the candidate\n"}))
        self.assert_rejected("--protected", "src/agent.py", contains="modified protected paths")

    def test_rejects_a_candidate_that_deletes_a_gate(self):
        self.install_plan(
            plan(writes={"src/improvement.py": IMPROVEMENT}, deletes=["tests/gate_test.py"])
        )
        self.assert_rejected(contains="weakened gates")

    def test_rejects_a_cosmetic_change(self):
        self.install_plan(plan(writes={"NOTES.md": "# tidied\n"}))
        self.assert_rejected(contains="change is cosmetic")

    def test_rejects_an_attempt_that_changed_nothing(self):
        self.install_plan(plan(claim=False))
        self.assert_rejected(contains="change is none")

    def test_rejects_a_claim_that_does_not_reproduce(self):
        self.install_plan(
            plan(
                writes={"src/improvement.py": IMPROVEMENT},
                verify="python3 tests/missing_check.py",
            )
        )
        self.assert_rejected(contains="did not reproduce")

    def test_rejects_a_candidate_that_breaks_the_existing_gate(self):
        self.install_plan(plan(writes={"src/improvement.py": IMPROVEMENT, "broken.marker": "x\n"}))
        self.assert_rejected(contains="gate failed")

    def test_rejects_an_always_passing_claim(self):
        self.install_plan(improvement_plan(plan(), verify="python3 -c 'pass'"))
        self.assert_rejected(contains="fail on the original")

    def test_existing_gate_cannot_be_weakened_by_a_same_length_rewrite(self):
        self.install_plan(plan(writes={"tests/gate_test.py": "print('ignored')\n" * 100}))
        self.assert_rejected(contains="weakened gates")

    def test_sandbox_denies_writes_to_authoritative_state(self):
        target = self.state / "escaped.txt"
        self.install_plan(plan(writes={str(target): "escaped"}))
        self.initialize()
        run = self.run_command("discover")["run"]
        self.assertNotEqual(run["returncode"], 0)
        self.assertFalse(target.exists())

    def test_model_call_overrun_is_rejected(self):
        self.install_plan(improvement_plan(plan(), model_requests=81))
        self.initialize("--max-model-calls", "80")
        run = self.run_command("discover")["run"]
        self.assertIn("model_requests", run["budget_breach"])

    def test_default_has_no_model_call_cap(self):
        self.install_plan(improvement_plan(plan(), model_requests=81))
        self.initialize()
        run = self.run_command("discover")["run"]
        self.assertEqual(run["returncode"], 0)
        self.assertIsNone(run["budget_breach"])
        session = json.loads(
            (self.generation_dir() / "work/discovery/home/session.json").read_text()
        )
        self.assertEqual(session["limits"]["UAGENT_MAX_STEPS"], "0")

    def test_subscription_without_model_cap_preserves_other_limits(self):
        declaration = {
            "non_billable": True,
            "cheap": True,
            "limits": {
                "max_sessions": 5,
                "max_model_calls": 0,
                "max_tool_calls": 32,
                "max_output_tokens_per_call": 8192,
                "max_session_seconds": 60,
            },
        }
        for authority_cap, extra, tool_calls, breach in (
            (0, (), 2, None),
            (8, (), 2, "model calls"),
            (0, ("--max-model-calls", "8"), 2, "model_requests"),
            (0, ("--max-tokens", "499"), 2, "token limit"),
            (0, (), 33, "tool"),
        ):
            with self.subTest(authority_cap=authority_cap, extra=extra, tool_calls=tool_calls):
                declaration["limits"]["max_model_calls"] = authority_cap
                self.authority.write_text(
                    json.dumps(
                        {"schema": "uagent.eval.cost-authority.v1", "routes": {ROUTE: declaration}}
                    )
                )
                self.install_plan(
                    improvement_plan(plan(), model_requests=81, tool_calls=tool_calls)
                )
                initialized = self.initialize(*extra, max_cost="0")
                run = self.run_command("discover")["run"]
                if breach:
                    self.assertIn(breach, run["budget_breach"])
                else:
                    self.assertEqual(run["returncode"], 0)
                    self.assertIsNone(run["budget_breach"])
                    session = json.loads(
                        (
                            self.generation_dir(initialized["generation"])
                            / "work/discovery/home/session.json"
                        ).read_text()
                    )
                    self.assertEqual(session["limits"]["UAGENT_MAX_STEPS"], "0")
                    self.assertEqual(session["limits"]["UAGENT_MAX_TOOL_CALLS"], "32")
                    self.assertEqual(session["limits"]["UAGENT_MAX_TOKENS"], "8192")
                    self.assertEqual(session["limits"]["UAGENT_MAX_TURN_SECONDS"], "60")

    def test_budget_exhaustion_leaves_the_incumbent_active(self):
        self.install_plan(improvement_plan(plan(), cost=0.50))
        self.initialize(max_cost="0.05")
        run = self.run_command("discover")["run"]
        self.assertIsNotNone(run["budget_breach"])
        verdict = self.run_command("gate")
        self.assertEqual(verdict["verdict"], "reject")
        self.assertTrue(any("exceeded" in reason for reason in verdict["reasons"]))

    def test_promotion_requires_approval_and_a_promote_verdict(self):
        self.install_plan(improvement_plan(plan(), cost=0.02))
        self.initialize()
        self.run_command("discover")
        self.run_command("gate")
        self.run_command("replay", "--pairs", "1")
        self.run_command("verdict")
        self.assertIn("error", self.run_command("promote", ok=False))


def summary(**overrides):
    base = {
        "trials": 1,
        "successes": 1,
        "eligible_runs": 1,
        "ineligible_reasons": [],
        "changed_paths": ["src/improvement.py"],
        "cost_usd": 0.02,
        "tokens": 500,
        "wall_ms": 1000,
        "tool_failures": 0,
    }
    base.update(overrides)
    return base


def pair(control_outcome=True, candidate_outcome=True, candidate_cost=0.02, changed=None):
    def record(outcome, cost):
        return {
            "cost_usd": cost,
            "tokens": 500,
            "wall_ms": 1000,
            "tool_failures": 0,
            "returncode": 0,
            "timed_out": False,
            "budget_breach": None,
            "evaluation": {
                "eligible": True,
                "outcome": outcome,
                "reasons": [],
                "change": {"changed": changed or ["src/improvement.py"]},
            },
        }

    return {
        "pair": 1,
        "control": record(control_outcome, 0.02),
        "candidate": record(candidate_outcome, candidate_cost),
    }


class VerdictTest(unittest.TestCase):
    tolerances = {"regression_pct": 10.0, "gain_pct": 10.0}

    def verdict(self, control, candidate):
        return compare(control, candidate, self.tolerances)["verdict"]

    def test_equal_outcome_with_materially_less_cost_wins(self):
        self.assertEqual(self.verdict(summary(), summary(cost_usd=0.01)), "promote")

    def test_equal_outcome_within_tolerance_is_inconclusive(self):
        self.assertEqual(self.verdict(summary(), summary(cost_usd=0.0201)), "inconclusive")

    def test_resource_regression_beyond_tolerance_rejects(self):
        self.assertEqual(self.verdict(summary(), summary(wall_ms=2000)), "reject")

    def test_capability_gain_wins_even_at_equal_cost(self):
        self.assertEqual(self.verdict(summary(successes=0), summary()), "promote")

    def test_lost_capability_rejects(self):
        self.assertEqual(self.verdict(summary(), summary(successes=0)), "reject")

    def test_fast_failure_is_never_a_win(self):
        # Half the cost, half the time, and nothing validated on either side.
        self.assertEqual(
            self.verdict(
                summary(successes=0),
                summary(successes=0, cost_usd=0.01, wall_ms=100),
            ),
            "inconclusive",
        )

    def test_ineligible_candidate_rejects(self):
        self.assertEqual(
            self.verdict(
                summary(),
                summary(eligible_runs=0, ineligible_reasons=["weakened gates: tests/gate_test.py"]),
            ),
            "reject",
        )

    def test_different_valid_improvements_are_incomparable(self):
        self.assertEqual(
            self.verdict(summary(), summary(changed_paths=["src/other.py"])),
            "incomparable",
        )

    def test_continuation_rejection_blocks_a_winning_replay(self):
        replay = {"pairs": [pair(candidate_cost=0.01)]}
        continuation = {"pairs": [pair(candidate_outcome=False)]}
        decision = decide(replay, continuation, self.tolerances)
        self.assertEqual(decision["verdict"], "reject")

    def test_promotion_requires_the_continuation_check(self):
        replay = {"pairs": [pair(candidate_cost=0.01)]}
        with self.assertRaises(ControllerError):
            decide(replay, None, self.tolerances)

    def test_continuation_without_a_winner_is_inconclusive(self):
        replay = {"pairs": [pair(candidate_cost=0.01)]}
        continuation = {"pairs": [pair(control_outcome=False, candidate_outcome=False)]}
        self.assertEqual(decide(replay, continuation, self.tolerances)["verdict"], "inconclusive")


if __name__ == "__main__":
    unittest.main()
