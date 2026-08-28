#!/usr/bin/env python3
"""Manage one bounded, reversible µAgent prompt-overlay experiment."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
import tempfile
from datetime import UTC, datetime
from typing import Any

from live_authority import AuthorityError, load_authority, normalize_route_authority

SCHEMA = "uagent.improvement.experiment.v1"
RESULTS_SCHEMA = "uagent.improvement.results.v1"
MAX_HYPOTHESIS_CHARS = 1_000
MAX_ID_CHARS = 80
MAX_TRIALS = 50
MAX_COST_USD = 100.0
MAX_OVERLAY_BYTES = 64 * 1024
MAX_STATE_FILE_BYTES = 256 * 1024
VARIANTS = ("control", "treatment")
PROMPT_SECTIONS = {
    "## Evidence",
    "## Tools",
    "## Changes",
    "## Delegation",
    "## Answer",
}


class ExperimentError(RuntimeError):
    pass


def now() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def digest(body: bytes) -> str:
    return hashlib.sha256(body).hexdigest()


def private_dir(path: pathlib.Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(0o700)


def atomic_write(path: pathlib.Path, body: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(body)
            output.flush()
            os.fchmod(output.fileno(), mode)
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_json(path: pathlib.Path, value: Any) -> None:
    atomic_write(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())


def read_json(path: pathlib.Path) -> Any:
    try:
        body = path.read_bytes()
        if len(body) > MAX_STATE_FILE_BYTES:
            raise ExperimentError(f"state file exceeds {MAX_STATE_FILE_BYTES} bytes: {path}")
        return json.loads(body)
    except FileNotFoundError as error:
        raise ExperimentError(f"missing {path}") from error
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ExperimentError(f"invalid JSON in {path}: {error}") from error


def require_number(value: Any, name: str, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ExperimentError(f"{name} must be a number")
    number = float(value)
    if not minimum <= number <= maximum:
        raise ExperimentError(f"{name} must be between {minimum:g} and {maximum:g}")
    return number


def require_keys(
    value: Any,
    name: str,
    required: set[str],
    optional: set[str] | None = None,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ExperimentError(f"{name} must be an object")
    optional = optional or set()
    missing = required - set(value)
    unknown = set(value) - required - optional
    if missing or unknown:
        raise ExperimentError(
            f"{name} fields are invalid; missing={sorted(missing)}, unknown={sorted(unknown)}"
        )
    return value


def validate_id(value: str) -> str:
    if not value or len(value) > MAX_ID_CHARS:
        raise ExperimentError(f"id must contain 1..{MAX_ID_CHARS} characters")
    if any(not (character.isalnum() or character in "-_") for character in value):
        raise ExperimentError("id may contain only letters, digits, '-' and '_'")
    return value


def default_id() -> str:
    return datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")


def root_path(arguments: argparse.Namespace) -> pathlib.Path:
    return arguments.root.expanduser().resolve()


def round_path(arguments: argparse.Namespace) -> pathlib.Path:
    return root_path(arguments) / "rounds" / validate_id(arguments.id)


def validate_overlay(path: pathlib.Path) -> bytes:
    try:
        body = path.read_bytes()
    except OSError as error:
        raise ExperimentError(f"cannot read overlay {path}: {error}") from error
    if not body or len(body) > MAX_OVERLAY_BYTES:
        raise ExperimentError(f"overlay must contain 1..{MAX_OVERLAY_BYTES} bytes")
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as error:
        raise ExperimentError(f"overlay is invalid JSON: {error}") from error
    if not isinstance(parsed, dict):
        raise ExperimentError("overlay must be a JSON object")
    unknown = set(parsed) - {"append", "replace"}
    if unknown:
        raise ExperimentError(f"overlay contains unsupported keys: {sorted(unknown)}")
    effective = False
    if "append" in parsed:
        if not isinstance(parsed["append"], str) or not parsed["append"]:
            raise ExperimentError("overlay.append must be a nonempty string")
        effective = True
    if "replace" in parsed:
        replace = parsed["replace"]
        if not isinstance(replace, dict) or not replace:
            raise ExperimentError("overlay.replace must be a nonempty object")
        unknown_sections = set(replace) - PROMPT_SECTIONS
        if unknown_sections:
            raise ExperimentError(
                f"overlay.replace contains unsupported sections: {sorted(unknown_sections)}"
            )
        if any(not isinstance(value, str) for value in replace.values()):
            raise ExperimentError("overlay.replace values must be strings")
        effective = True
    if not effective:
        raise ExperimentError("overlay must define append or replace")
    return body


def validate_review(value: Any) -> None:
    if value is None:
        return
    review = require_keys(
        value,
        "experiment.review",
        {"reviewed_at", "verdict", "success", "guardrails", "spent_usd"},
    )
    if review["verdict"] not in ("pass", "reject", "inconclusive"):
        raise ExperimentError("experiment.review.verdict is invalid")
    if not isinstance(review["reviewed_at"], str) or len(review["reviewed_at"]) > 40:
        raise ExperimentError("experiment.review.reviewed_at is invalid")
    success = require_keys(
        review["success"],
        "experiment.review.success",
        {"control", "treatment", "delta"},
    )
    for name, number in success.items():
        if isinstance(number, bool) or not isinstance(number, int):
            raise ExperimentError(f"experiment.review.success.{name} is invalid")
    guardrails = require_keys(
        review["guardrails"],
        "experiment.review.guardrails",
        {"tokens", "wall_ms", "tool_failures"},
    )
    for name, raw in guardrails.items():
        guardrail = require_keys(
            raw,
            f"experiment.review.guardrails.{name}",
            {"control_mean", "treatment_mean", "regression_pct", "passed"},
        )
        require_number(guardrail["control_mean"], f"{name}.control_mean", 0, 100_000_000)
        require_number(guardrail["treatment_mean"], f"{name}.treatment_mean", 0, 100_000_000)
        regression = guardrail["regression_pct"]
        if regression is not None:
            require_number(regression, f"{name}.regression_pct", -100.0, 100_000_000)
        if not isinstance(guardrail["passed"], bool):
            raise ExperimentError(f"experiment.review.guardrails.{name}.passed is invalid")
    require_number(review["spent_usd"], "experiment.review.spent_usd", 0, MAX_COST_USD)


def load_round(path: pathlib.Path) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest = require_keys(
        read_json(path / "experiment.json"),
        "experiment",
        {
            "schema",
            "id",
            "created_at",
            "updated_at",
            "status",
            "hypothesis",
            "primary_metric",
            "cohort",
            "limits",
            "decision",
            "overlay",
            "activation",
            "review",
        },
        {"overlay_written_at", "rolled_back_at"},
    )
    results = require_keys(
        read_json(path / "results.json"),
        "results",
        {"schema", "records"},
    )
    if manifest.get("schema") != SCHEMA:
        raise ExperimentError(f"unsupported experiment schema in {path}")
    if results.get("schema") != RESULTS_SCHEMA:
        raise ExperimentError(f"unsupported results schema in {path}")
    records = results.get("records")
    if not isinstance(records, list):
        raise ExperimentError("results.records must be a list")
    limits = require_keys(
        manifest.get("limits"),
        "experiment.limits",
        {"trials_per_variant", "max_cost_usd", "max_overlay_bytes"},
    )
    trials = limits.get("trials_per_variant")
    if isinstance(trials, bool) or not isinstance(trials, int) or not 1 <= trials <= MAX_TRIALS:
        raise ExperimentError(f"trials_per_variant must be between 1 and {MAX_TRIALS}")
    require_number(limits.get("max_cost_usd"), "max_cost_usd", 0.0, MAX_COST_USD)
    if limits["max_overlay_bytes"] != MAX_OVERLAY_BYTES:
        raise ExperimentError("experiment.limits.max_overlay_bytes is invalid")
    status = manifest.get("status")
    if status not in ("proposed", "reviewed", "overlay_written", "rolled_back"):
        raise ExperimentError("experiment.status is invalid")
    try:
        if validate_id(manifest.get("id")) != path.name:
            raise ExperimentError("experiment.id does not match its directory")
    except TypeError as error:
        raise ExperimentError("experiment.id is invalid") from error
    if manifest.get("primary_metric") != "success":
        raise ExperimentError("experiment.primary_metric is invalid")
    for timestamp in ("created_at", "updated_at"):
        value = manifest.get(timestamp)
        if not isinstance(value, str) or not 1 <= len(value) <= 40:
            raise ExperimentError(f"experiment.{timestamp} is invalid")
    hypothesis = manifest.get("hypothesis")
    if not isinstance(hypothesis, str) or not 1 <= len(hypothesis) <= MAX_HYPOTHESIS_CHARS:
        raise ExperimentError("experiment.hypothesis is invalid")
    cohort = require_keys(
        manifest.get("cohort"),
        "experiment.cohort",
        {"model", "effort"},
        {"cost_basis", "authority"},
    )
    if (
        not isinstance(cohort.get("model"), str)
        or not 1 <= len(cohort["model"]) <= 256
        or not isinstance(cohort.get("effort"), str)
        or not 1 <= len(cohort["effort"]) <= 64
    ):
        raise ExperimentError("experiment.cohort is invalid")
    cost_basis = cohort.get("cost_basis", "reported-cost")
    if cost_basis not in ("reported-cost", "non-billable-cheap"):
        raise ExperimentError("experiment.cohort.cost_basis is invalid")
    authority_receipt = cohort.get("authority")
    if cost_basis == "reported-cost":
        if authority_receipt is not None:
            raise ExperimentError("reported-cost experiment has a cheap authority receipt")
    else:
        receipt = require_keys(
            authority_receipt,
            "experiment.cohort.authority",
            {"route", "mode", "limits", "sha256"},
        )
        if receipt["route"] != cohort["model"] or receipt["mode"] != "non-billable-cheap":
            raise ExperimentError("experiment.cohort.authority route or mode is invalid")
        sha256 = receipt["sha256"]
        if (
            not isinstance(sha256, str)
            or len(sha256) != 64
            or any(character not in "0123456789abcdef" for character in sha256)
        ):
            raise ExperimentError("experiment.cohort.authority sha256 is invalid")
        try:
            normalized = normalize_route_authority(
                receipt["route"],
                {"non_billable": True, "cheap": True, "limits": receipt["limits"]},
            )
        except AuthorityError as error:
            raise ExperimentError(f"experiment.cohort.authority is invalid: {error}") from error
        if normalized["limits"] != receipt["limits"]:
            raise ExperimentError("experiment.cohort.authority limits are not normalized")
    decision = require_keys(
        manifest.get("decision"),
        "experiment.decision",
        {"min_success_delta", "max_guardrail_regression_pct"},
    )
    minimum_delta = decision.get("min_success_delta")
    if (
        isinstance(minimum_delta, bool)
        or not isinstance(minimum_delta, int)
        or not 1 <= minimum_delta <= trials
    ):
        raise ExperimentError("experiment.decision.min_success_delta is invalid")
    require_number(
        decision.get("max_guardrail_regression_pct"),
        "max_guardrail_regression_pct",
        0.0,
        100.0,
    )
    max_cost = float(limits["max_cost_usd"])
    if cost_basis == "reported-cost" and max_cost <= 0:
        raise ExperimentError("reported-cost experiments require a positive max_cost_usd")
    if cost_basis == "non-billable-cheap" and max_cost != 0:
        raise ExperimentError("non-billable-cheap experiments require max_cost_usd=0")
    if len(records) > trials * len(VARIANTS):
        raise ExperimentError("results contain more records than the declared trial limit")
    seen = set()
    for record in records:
        if not isinstance(record, dict) or record.get("variant") not in VARIANTS:
            raise ExperimentError("results contain an invalid record")
        require_keys(
            record,
            "result record",
            {
                "variant",
                "trial",
                "task_id",
                "success",
                "cost_usd",
                "tokens",
                "wall_ms",
                "tool_failures",
            },
            {"authority_sha256"},
        )
        trial = record.get("trial")
        if isinstance(trial, bool) or not isinstance(trial, int) or not 1 <= trial <= trials:
            raise ExperimentError("results contain an invalid trial number")
        key = (record["variant"], trial)
        if key in seen:
            raise ExperimentError("results contain a duplicate trial")
        seen.add(key)
        try:
            validate_id(record.get("task_id"))
        except (ExperimentError, TypeError) as error:
            raise ExperimentError("results contain an invalid task_id") from error
        if not isinstance(record.get("success"), bool):
            raise ExperimentError("results contain an invalid success value")
        recorded_authority = record.get("authority_sha256")
        if cost_basis == "non-billable-cheap":
            if recorded_authority != cohort["authority"]["sha256"]:
                raise ExperimentError("cheap result authority digest does not match the round")
        elif recorded_authority is not None:
            raise ExperimentError("reported-cost result has a cheap authority digest")
        require_number(record.get("cost_usd"), "record.cost_usd", 0.0, MAX_COST_USD)
        require_number(record.get("tokens"), "record.tokens", 0, 100_000_000)
        require_number(record.get("wall_ms"), "record.wall_ms", 0, 86_400_000)
        require_number(record.get("tool_failures"), "record.tool_failures", 0, 1_000_000)
    if sum(float(record["cost_usd"]) for record in records) > max_cost + 1e-9:
        raise ExperimentError("results exceed the declared aggregate cost limit")
    overlay = require_keys(
        manifest.get("overlay"),
        "experiment.overlay",
        {"candidate_file", "candidate_sha256", "candidate_bytes"},
    )
    if (
        overlay.get("candidate_file") != "candidate-overlay.json"
        or not isinstance(overlay.get("candidate_sha256"), str)
        or len(overlay["candidate_sha256"]) != 64
        or isinstance(overlay.get("candidate_bytes"), bool)
        or not isinstance(overlay.get("candidate_bytes"), int)
        or not 1 <= overlay["candidate_bytes"] <= MAX_OVERLAY_BYTES
    ):
        raise ExperimentError("experiment.overlay is invalid")
    activation = require_keys(
        manifest.get("activation"),
        "experiment.activation",
        {
            "config_scope",
            "target_overlay",
            "target_existed",
            "target_sha256",
            "target_mode",
            "written_mode",
            "previous_setting",
        },
    )
    if (
        activation.get("config_scope") not in ("user", "project")
        or not isinstance(activation.get("target_overlay"), str)
        or not 1 <= len(activation["target_overlay"]) <= 4096
        or not isinstance(activation.get("target_existed"), bool)
        or (
            activation.get("previous_setting") is not None
            and not isinstance(activation.get("previous_setting"), str)
        )
    ):
        raise ExperimentError("experiment.activation is invalid")
    written_mode = activation.get("written_mode")
    if (
        isinstance(written_mode, bool)
        or not isinstance(written_mode, int)
        or not 0 <= written_mode <= 0o7777
    ):
        raise ExperimentError("experiment.activation.written_mode is invalid")
    if activation["target_existed"]:
        target_digest = activation.get("target_sha256")
        target_mode = activation.get("target_mode")
        if (
            not isinstance(target_digest, str)
            or len(target_digest) != 64
            or isinstance(target_mode, bool)
            or not isinstance(target_mode, int)
            or not 0 <= target_mode <= 0o7777
        ):
            raise ExperimentError("experiment.activation target snapshot is invalid")
    elif activation.get("target_sha256") is not None or activation.get("target_mode") is not None:
        raise ExperimentError("experiment.activation has metadata for an absent target")
    validate_review(manifest.get("review"))
    if status == "proposed" and manifest["review"] is not None:
        raise ExperimentError("proposed experiment may not contain a review")
    if status != "proposed" and manifest["review"] is None:
        raise ExperimentError("reviewed experiment is missing its review")
    candidate = path / "candidate-overlay.json"
    try:
        candidate_body = candidate.read_bytes()
    except OSError as error:
        raise ExperimentError(f"cannot read candidate overlay: {error}") from error
    if (
        len(candidate_body) != overlay["candidate_bytes"]
        or digest(candidate_body) != overlay["candidate_sha256"]
    ):
        raise ExperimentError("candidate overlay digest or size mismatch")
    return manifest, results


def save_manifest(path: pathlib.Path, manifest: dict[str, Any]) -> None:
    manifest["updated_at"] = now()
    write_json(path / "experiment.json", manifest)


def config_proposal(manifest: dict[str, Any], rollback: bool = False) -> dict[str, Any]:
    activation = manifest["activation"]
    previous = activation["previous_setting"]
    if rollback:
        change = (
            {"key": "UAGENT_PROMPT_OVERLAY", "operation": "unset"}
            if previous is None
            else {"key": "UAGENT_PROMPT_OVERLAY", "operation": "set", "value": previous}
        )
    else:
        change = {
            "key": "UAGENT_PROMPT_OVERLAY",
            "operation": "set",
            "value": activation["target_overlay"],
        }
    return {
        "instruction": "Apply only through the human-approved uagent_configure tool.",
        "tool": "uagent_configure",
        "scope": activation["config_scope"],
        "changes": [change],
    }


def command_init(arguments: argparse.Namespace) -> dict[str, Any]:
    if not arguments.hypothesis or len(arguments.hypothesis) > MAX_HYPOTHESIS_CHARS:
        raise ExperimentError(f"hypothesis must contain 1..{MAX_HYPOTHESIS_CHARS} characters")
    if not arguments.model or len(arguments.model) > 256:
        raise ExperimentError("model must contain 1..256 characters")
    if not arguments.effort or len(arguments.effort) > 64:
        raise ExperimentError("effort must contain 1..64 characters")
    if arguments.previous_setting is not None and len(arguments.previous_setting) > 4096:
        raise ExperimentError("previous_setting exceeds 4096 characters")
    trials = int(require_number(arguments.trials, "trials", 1, MAX_TRIALS))
    max_cost = require_number(arguments.max_cost, "max_cost", 0.0, MAX_COST_USD)
    if arguments.cost_basis == "reported-cost" and max_cost <= 0:
        raise ExperimentError("reported-cost experiments require a positive max_cost")
    if arguments.cost_basis == "non-billable-cheap" and max_cost != 0:
        raise ExperimentError("non-billable-cheap experiments require max_cost=0")
    authority_receipt = None
    if arguments.cost_basis == "non-billable-cheap":
        if arguments.cost_authority is None:
            raise ExperimentError("non-billable-cheap experiments require --cost-authority")
        try:
            authority = load_authority(
                arguments.cost_authority.expanduser().resolve(), [arguments.model]
            )
        except AuthorityError as error:
            raise ExperimentError(f"invalid cheap-route authority: {error}") from error
        declaration = authority["routes"][arguments.model]
        if declaration["mode"] != "non-billable-cheap":
            raise ExperimentError("cost authority does not declare this route non-billable-cheap")
        authority_receipt = {
            "route": arguments.model,
            "mode": declaration["mode"],
            "limits": declaration["limits"],
            "sha256": authority["sha256"],
        }
    elif arguments.cost_authority is not None:
        raise ExperimentError("--cost-authority is only stored for non-billable-cheap rounds")
    min_delta = int(require_number(arguments.min_success_delta, "min_success_delta", 1, trials))
    max_regression = require_number(
        arguments.max_guardrail_regression_pct,
        "max_guardrail_regression_pct",
        0.0,
        100.0,
    )
    source = arguments.overlay.expanduser().resolve()
    body = validate_overlay(source)
    target = pathlib.Path(os.path.abspath(arguments.target_overlay.expanduser()))
    if len(str(target)) > 4096:
        raise ExperimentError("target overlay path exceeds 4096 characters")
    if target.is_symlink():
        raise ExperimentError("target overlay may not be a symbolic link")
    if target.exists() and not target.is_file():
        raise ExperimentError("target overlay must be a regular file or not exist")
    target_existed = target.is_file()
    target_body = target.read_bytes() if target_existed else b""
    target_mode = target.stat().st_mode & 0o7777 if target_existed else None

    path = round_path(arguments)
    if path.exists():
        raise ExperimentError(f"round already exists: {path}")
    root = root_path(arguments)
    rounds = root / "rounds"
    private_dir(root)
    private_dir(rounds)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".init.", dir=rounds))
    temporary.chmod(0o700)
    candidate = temporary / "candidate-overlay.json"

    manifest = {
        "schema": SCHEMA,
        "id": arguments.id,
        "created_at": now(),
        "updated_at": now(),
        "status": "proposed",
        "hypothesis": arguments.hypothesis,
        "primary_metric": "success",
        "cohort": {
            "model": arguments.model,
            "effort": arguments.effort,
            "cost_basis": arguments.cost_basis,
            **({"authority": authority_receipt} if authority_receipt is not None else {}),
        },
        "limits": {
            "trials_per_variant": trials,
            "max_cost_usd": max_cost,
            "max_overlay_bytes": MAX_OVERLAY_BYTES,
        },
        "decision": {
            "min_success_delta": min_delta,
            "max_guardrail_regression_pct": max_regression,
        },
        "overlay": {
            "candidate_file": "candidate-overlay.json",
            "candidate_sha256": digest(body),
            "candidate_bytes": len(body),
        },
        "activation": {
            "config_scope": arguments.config_scope,
            "target_overlay": str(target),
            "target_existed": target_existed,
            "target_sha256": digest(target_body) if target_existed else None,
            "target_mode": target_mode,
            "written_mode": 0o600,
            "previous_setting": arguments.previous_setting,
        },
        "review": None,
    }
    try:
        atomic_write(candidate, body)
        if target_existed:
            atomic_write(temporary / "rollback-overlay.bin", target_body)
        write_json(temporary / "experiment.json", manifest)
        write_json(temporary / "results.json", {"schema": RESULTS_SCHEMA, "records": []})
        os.replace(temporary, path)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return {
        "round": str(path),
        "status": "proposed",
        "candidate_sha256": manifest["overlay"]["candidate_sha256"],
        "next": f"record {trials} control and {trials} treatment trials",
    }


def command_record(arguments: argparse.Namespace) -> dict[str, Any]:
    path = round_path(arguments)
    manifest, results = load_round(path)
    if manifest["status"] != "proposed":
        raise ExperimentError("records may be added only while status is proposed")
    if arguments.model != manifest["cohort"]["model"]:
        raise ExperimentError("trial model differs from the pre-registered cohort")
    if arguments.effort != manifest["cohort"]["effort"]:
        raise ExperimentError("trial effort differs from the pre-registered cohort")
    task_id = validate_id(arguments.task_id)
    maximum = manifest["limits"]["trials_per_variant"]
    if not 1 <= arguments.trial <= maximum:
        raise ExperimentError(f"trial must be between 1 and {maximum}")
    cost = require_number(arguments.cost, "cost", 0.0, MAX_COST_USD)
    cost_basis = manifest["cohort"].get("cost_basis", "reported-cost")
    if cost_basis == "non-billable-cheap":
        expected_authority = manifest["cohort"]["authority"]["sha256"]
        if arguments.authority_sha256 != expected_authority:
            raise ExperimentError("trial authority digest differs from the pre-registered round")
    elif arguments.authority_sha256 is not None:
        raise ExperimentError("reported-cost trials may not supply --authority-sha256")
    if cost_basis == "non-billable-cheap" and cost != 0:
        raise ExperimentError("non-billable-cheap trial cost must be 0")
    tokens = int(require_number(arguments.tokens, "tokens", 0, 100_000_000))
    wall_ms = int(require_number(arguments.wall_ms, "wall_ms", 0, 86_400_000))
    failures = int(require_number(arguments.tool_failures, "tool_failures", 0, 1_000_000))
    key = (arguments.variant, arguments.trial)
    if any((record.get("variant"), record.get("trial")) == key for record in results["records"]):
        raise ExperimentError(f"duplicate result for {arguments.variant} trial {arguments.trial}")
    spent = sum(float(record["cost_usd"]) for record in results["records"])
    cap = float(manifest["limits"]["max_cost_usd"])
    if spent + cost > cap + 1e-9:
        raise ExperimentError(
            f"aggregate cost ${spent + cost:.6f} exceeds ${cap:.6f} experiment limit"
        )
    record = {
        "variant": arguments.variant,
        "trial": arguments.trial,
        "task_id": task_id,
        "success": arguments.success == "yes",
        "cost_usd": cost,
        "tokens": tokens,
        "wall_ms": wall_ms,
        "tool_failures": failures,
        **(
            {"authority_sha256": arguments.authority_sha256}
            if arguments.authority_sha256 is not None
            else {}
        ),
    }
    results["records"].append(record)
    results["records"].sort(key=lambda item: (VARIANTS.index(item["variant"]), item["trial"]))
    write_json(path / "results.json", results)
    return {"recorded": record, "spent_usd": spent + cost, "remaining_usd": cap - spent - cost}


def mean(records: list[dict[str, Any]], field: str) -> float:
    return sum(float(record[field]) for record in records) / len(records)


def regression_pct(control: float, treatment: float) -> float:
    if control == 0:
        return 0.0 if treatment == 0 else float("inf")
    return (treatment - control) * 100.0 / control


def review_round(manifest: dict[str, Any], results: dict[str, Any]) -> dict[str, Any]:
    expected = manifest["limits"]["trials_per_variant"]
    groups = {
        variant: [record for record in results["records"] if record["variant"] == variant]
        for variant in VARIANTS
    }
    for variant, records in groups.items():
        if len(records) != expected or {record["trial"] for record in records} != set(
            range(1, expected + 1)
        ):
            raise ExperimentError(f"{variant} requires exactly trials 1..{expected}")
    control_by_trial = {record["trial"]: record for record in groups["control"]}
    treatment_by_trial = {record["trial"]: record for record in groups["treatment"]}
    for trial in range(1, expected + 1):
        if control_by_trial[trial]["task_id"] != treatment_by_trial[trial]["task_id"]:
            raise ExperimentError(f"control and treatment task_id differ for trial {trial}")
    control_success = sum(record["success"] for record in groups["control"])
    treatment_success = sum(record["success"] for record in groups["treatment"])
    delta = treatment_success - control_success
    allowed = float(manifest["decision"]["max_guardrail_regression_pct"])
    guardrails = {}
    for field in ("tokens", "wall_ms", "tool_failures"):
        baseline = mean(groups["control"], field)
        treatment = mean(groups["treatment"], field)
        change = regression_pct(baseline, treatment)
        guardrails[field] = {
            "control_mean": baseline,
            "treatment_mean": treatment,
            "regression_pct": None if change == float("inf") else change,
            "passed": change <= allowed,
        }
    guardrails_passed = all(value["passed"] for value in guardrails.values())
    if delta < 0 or not guardrails_passed:
        verdict = "reject"
    elif delta >= manifest["decision"]["min_success_delta"]:
        verdict = "pass"
    else:
        verdict = "inconclusive"
    return {
        "reviewed_at": now(),
        "verdict": verdict,
        "success": {
            "control": control_success,
            "treatment": treatment_success,
            "delta": delta,
        },
        "guardrails": guardrails,
        "spent_usd": sum(float(record["cost_usd"]) for record in results["records"]),
    }


def command_review(arguments: argparse.Namespace) -> dict[str, Any]:
    path = round_path(arguments)
    manifest, results = load_round(path)
    if manifest["status"] not in ("proposed", "reviewed"):
        raise ExperimentError(f"cannot review experiment with status {manifest['status']}")
    review = review_round(manifest, results)
    manifest["review"] = review
    manifest["status"] = "reviewed"
    save_manifest(path, manifest)
    output = {"round": str(path), "review": review}
    if review["verdict"] == "pass":
        output["activation_proposal"] = config_proposal(manifest)
        output["next"] = (
            f"activate with explicit approval: activate --id {manifest['id']} --approve"
        )
    else:
        output["next"] = "do not activate; revise the hypothesis or end the round"
    return output


def command_activate(arguments: argparse.Namespace) -> dict[str, Any]:
    if not arguments.approve:
        raise ExperimentError("activation requires --approve")
    path = round_path(arguments)
    manifest, _ = load_round(path)
    if manifest["status"] != "reviewed" or (manifest.get("review") or {}).get("verdict") != "pass":
        raise ExperimentError("only a reviewed passing experiment may write the overlay")
    candidate = path / manifest["overlay"]["candidate_file"]
    body = candidate.read_bytes()
    if digest(body) != manifest["overlay"]["candidate_sha256"]:
        raise ExperimentError("candidate overlay digest changed after initialization")
    target = pathlib.Path(manifest["activation"]["target_overlay"])
    expected_existing = manifest["activation"]["target_existed"]
    if target.is_symlink():
        raise ExperimentError("target overlay became a symbolic link; refusing activation")
    if expected_existing:
        if (
            not target.is_file()
            or digest(target.read_bytes()) != manifest["activation"]["target_sha256"]
            or target.stat().st_mode & 0o7777 != manifest["activation"]["target_mode"]
        ):
            raise ExperimentError("target overlay content or mode changed after initialization")
    elif target.exists():
        raise ExperimentError("target overlay appeared after initialization")
    state_root = root_path(arguments)
    if target.is_relative_to(state_root):
        private_dir(target.parent)
    atomic_write(target, body, mode=manifest["activation"]["written_mode"])
    manifest["status"] = "overlay_written"
    manifest["overlay_written_at"] = now()
    try:
        save_manifest(path, manifest)
    except Exception:
        if expected_existing:
            snapshot = path / "rollback-overlay.bin"
            atomic_write(
                target,
                snapshot.read_bytes(),
                mode=manifest["activation"]["target_mode"],
            )
        else:
            target.unlink(missing_ok=True)
        raise
    return {
        "round": str(path),
        "status": "overlay_written",
        "overlay_written": str(target),
        "configuration_proposal": config_proposal(manifest),
        "warning": "the runner did not change µAgent configuration",
    }


def command_rollback(arguments: argparse.Namespace) -> dict[str, Any]:
    path = round_path(arguments)
    manifest, _ = load_round(path)
    if manifest["status"] != "overlay_written":
        raise ExperimentError("only a written experiment overlay may be rolled back")
    target = pathlib.Path(manifest["activation"]["target_overlay"])
    candidate_digest = manifest["overlay"]["candidate_sha256"]
    if target.is_symlink():
        raise ExperimentError("active overlay became a symbolic link; refusing rollback")
    if (
        not target.is_file()
        or digest(target.read_bytes()) != candidate_digest
        or target.stat().st_mode & 0o7777 != manifest["activation"]["written_mode"]
    ):
        raise ExperimentError("active overlay content or mode changed externally")
    if manifest["activation"]["target_existed"]:
        snapshot = path / "rollback-overlay.bin"
        body = snapshot.read_bytes()
        if digest(body) != manifest["activation"]["target_sha256"]:
            raise ExperimentError("rollback snapshot digest mismatch")
        atomic_write(target, body, mode=manifest["activation"]["target_mode"])
        action = "restored"
    else:
        target.unlink()
        action = "removed"
    manifest["status"] = "rolled_back"
    manifest["rolled_back_at"] = now()
    try:
        save_manifest(path, manifest)
    except Exception:
        atomic_write(
            target,
            (path / manifest["overlay"]["candidate_file"]).read_bytes(),
            mode=manifest["activation"]["written_mode"],
        )
        raise
    return {
        "round": str(path),
        "status": "rolled_back",
        "overlay": {"path": str(target), "action": action},
        "configuration_proposal": config_proposal(manifest, rollback=True),
        "warning": "the runner did not change µAgent configuration",
    }


def command_status(arguments: argparse.Namespace) -> dict[str, Any]:
    path = round_path(arguments)
    manifest, results = load_round(path)
    counts = {
        variant: sum(record["variant"] == variant for record in results["records"])
        for variant in VARIANTS
    }
    return {
        "round": str(path),
        "id": manifest["id"],
        "status": manifest["status"],
        "hypothesis": manifest["hypothesis"],
        "cohort": manifest["cohort"],
        "records": counts,
        "required_per_variant": manifest["limits"]["trials_per_variant"],
        "spent_usd": sum(float(record["cost_usd"]) for record in results["records"]),
        "max_cost_usd": manifest["limits"]["max_cost_usd"],
        "review": manifest.get("review"),
    }


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(description=__doc__)
    top.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path("~/.uagent/improve"),
        help="private improvement state root",
    )
    commands = top.add_subparsers(dest="command", required=True)

    init = commands.add_parser("init", help="pre-register one prompt-overlay experiment")
    init.add_argument("--id", default=default_id())
    init.add_argument("--hypothesis", required=True)
    init.add_argument("--overlay", required=True, type=pathlib.Path)
    init.add_argument("--target-overlay", required=True, type=pathlib.Path)
    init.add_argument("--model", required=True)
    init.add_argument("--effort", default="provider-default")
    init.add_argument("--trials", type=int, default=5)
    init.add_argument(
        "--cost-basis",
        choices=("reported-cost", "non-billable-cheap"),
        default="reported-cost",
    )
    init.add_argument("--cost-authority", type=pathlib.Path)
    init.add_argument("--max-cost", type=float, required=True)
    init.add_argument("--min-success-delta", type=int, default=1)
    init.add_argument("--max-guardrail-regression-pct", type=float, default=10.0)
    init.add_argument("--config-scope", choices=("user", "project"), default="user")
    init.add_argument("--previous-setting")
    init.set_defaults(function=command_init)

    for name in ("status", "review", "rollback"):
        command = commands.add_parser(name)
        command.add_argument("--id", required=True)
        command.set_defaults(function=globals()[f"command_{name}"])

    record = commands.add_parser("record", help="record one bounded trial result")
    record.add_argument("--id", required=True)
    record.add_argument("--variant", choices=VARIANTS, required=True)
    record.add_argument("--trial", type=int, required=True)
    record.add_argument("--task-id", required=True)
    record.add_argument("--model", required=True)
    record.add_argument("--effort", required=True)
    record.add_argument("--success", choices=("yes", "no"), required=True)
    record.add_argument("--cost", type=float, required=True)
    record.add_argument("--authority-sha256")
    record.add_argument("--tokens", type=int, required=True)
    record.add_argument("--wall-ms", type=int, required=True)
    record.add_argument("--tool-failures", type=int, required=True)
    record.set_defaults(function=command_record)

    activate = commands.add_parser("activate", help="write the reviewed overlay, not config")
    activate.add_argument("--id", required=True)
    activate.add_argument("--approve", action="store_true")
    activate.set_defaults(function=command_activate)
    return top


def main() -> int:
    arguments = parser().parse_args()
    try:
        output = arguments.function(arguments)
    except (ExperimentError, OSError) as error:
        print(f"experiment error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
