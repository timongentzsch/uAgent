#!/usr/bin/env python3
"""Shared validation for live eval route authority."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
from typing import Any

SCHEMA = "uagent.eval.cost-authority.v1"
MAX_AUTHORITY_BYTES = 64 * 1024
CHEAP_LIMIT_CEILINGS = {
    "max_sessions": 12,
    "max_model_calls": 8,
    "max_tool_calls": 32,
    "max_output_tokens_per_call": 8192,
    "max_session_seconds": 300,
}


class AuthorityError(RuntimeError):
    pass


def normalize_route_authority(
    model: str, declaration: Any, *, allow_unlimited_model_calls: bool = False
) -> dict[str, Any]:
    if not isinstance(declaration, dict):
        raise AuthorityError(f"{model} has no route declaration")
    for name in ("reports_cost", "enforces_hard_budget", "non_billable", "cheap"):
        if name in declaration and not isinstance(declaration[name], bool):
            raise AuthorityError(f"{model} {name} must be a JSON boolean")
    reported = declaration.get("reports_cost") is True
    hard_budget = declaration.get("enforces_hard_budget") is True
    non_billable = declaration.get("non_billable") is True
    cheap = declaration.get("cheap") is True
    if reported and hard_budget:
        if non_billable or cheap:
            raise AuthorityError(f"{model} mixes reported-cost and non-billable modes")
        return {"mode": "reported-cost"}
    if not non_billable or not cheap:
        raise AuthorityError(
            f"{model} must declare reported cost with a hard USD budget, or explicitly "
            "declare both non_billable and cheap as JSON true"
        )
    limits = declaration.get("limits")
    if not isinstance(limits, dict) or set(limits) != set(CHEAP_LIMIT_CEILINGS):
        raise AuthorityError(
            f"{model} cheap authority must declare exactly {sorted(CHEAP_LIMIT_CEILINGS)}"
        )
    normalized_limits = {}
    for name, ceiling in CHEAP_LIMIT_CEILINGS.items():
        value = limits.get(name)
        minimum = 0 if name == "max_model_calls" and allow_unlimited_model_calls else 1
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= ceiling:
            raise AuthorityError(f"{model} {name} must be an integer in {minimum}..{ceiling}")
        normalized_limits[name] = value
    return {"mode": "non-billable-cheap", "limits": normalized_limits}


def load_authority(
    path: Path | None, models: list[str], *, allow_unlimited_model_calls: bool = False
) -> dict[str, Any]:
    if path is None:
        raise AuthorityError(
            "--cost-authority is required; each route must declare reported cost with a hard "
            "USD budget or explicit non-billable cheap limits"
        )
    try:
        body = path.read_bytes()
        if len(body) > MAX_AUTHORITY_BYTES:
            raise AuthorityError(f"cost authority exceeds {MAX_AUTHORITY_BYTES} bytes")
        authority = json.loads(body)
    except (OSError, json.JSONDecodeError) as error:
        raise AuthorityError(f"invalid cost authority: {error}") from error
    if not isinstance(authority, dict):
        raise AuthorityError("cost authority must be an object")
    if authority.get("schema") != SCHEMA:
        raise AuthorityError("unknown cost-authority schema")
    routes = authority.get("routes")
    if not isinstance(routes, dict):
        raise AuthorityError("cost authority has no routes object")
    normalized = {
        model: normalize_route_authority(
            model, routes.get(model), allow_unlimited_model_calls=allow_unlimited_model_calls
        )
        for model in models
    }
    return {
        "schema": SCHEMA,
        "routes": normalized,
        "sha256": hashlib.sha256(body).hexdigest(),
    }


def apply_authority(env: dict[str, str], declaration: dict[str, Any]) -> None:
    """Impose an explicitly declared cheap route's hard limits on a child."""
    if declaration["mode"] != "non-billable-cheap":
        return
    limits = declaration["limits"]
    env.update(
        {
            "UAGENT_MAX_STEPS": str(limits["max_model_calls"]),
            "UAGENT_MAX_TOOL_CALLS": str(limits["max_tool_calls"]),
            "UAGENT_MAX_TOKENS": str(limits["max_output_tokens_per_call"]),
            "UAGENT_MAX_TURN_SECONDS": str(limits["max_session_seconds"]),
            "UAGENT_REQUEST_TIMEOUT": str(limits["max_session_seconds"]),
            "UAGENT_FIRST_EVENT_TIMEOUT": str(limits["max_session_seconds"]),
            "UAGENT_STREAM_IDLE_TIMEOUT": str(limits["max_session_seconds"]),
            "UAGENT_MAX_TURN_COST": "0",
            "UAGENT_SESSION_BUDGET": "0",
            "UAGENT_OPENROUTER_FALLBACKS": "0",
        }
    )


def account_reported_cost(result: dict[str, Any], spent: float, cap: float) -> float:
    """Add one run's provider-reported cost to the aggregate, or refuse."""
    usage = result.get("usage") if isinstance(result.get("usage"), dict) else {}
    reported = bool(usage.get("cost_reported")) or bool(result.get("cost_reported"))
    route = result.get("model") or result.get("route") or "route"
    if not reported:
        raise AuthorityError(f"blocked after {route}: provider cost was unavailable")
    cost = float(usage.get("cost") or result.get("cost_usd") or 0)
    if not math.isfinite(cost) or cost < 0:
        raise AuthorityError("blocked: provider reported an invalid cost")
    updated = spent + cost
    if updated > cap + 1e-9:
        raise AuthorityError(
            f"stopped: reported aggregate cost ${updated:.6f} exceeded the ${cap:.6f} ceiling"
        )
    return updated


def account_result(
    result: dict[str, Any], declaration: dict[str, Any], spent: float, cap: float
) -> float:
    """Check one finished run against the authority it was launched under."""
    if declaration["mode"] == "reported-cost":
        return account_reported_cost(result, spent, cap)
    limits = declaration["limits"]
    usage = result.get("usage") if isinstance(result.get("usage"), dict) else {}
    route = result.get("model") or result.get("route") or "route"
    checks = {
        "model calls": (int(result.get("model_requests") or 0), limits["max_model_calls"]),
        "tool calls": (int(result.get("tool_calls") or 0), limits["max_tool_calls"]),
        "output tokens": (
            int(usage.get("output") or result.get("output_tokens") or 0),
            limits["max_model_calls"] * limits["max_output_tokens_per_call"],
        ),
        "session milliseconds": (
            int(float(result.get("elapsed_seconds") or 0) * 1000),
            limits["max_session_seconds"] * 1000,
        ),
    }
    for name, (used, maximum) in checks.items():
        if maximum and used > maximum:
            raise AuthorityError(
                f"blocked after {route}: {name} {used} exceeded the enforced cheap-route "
                f"limit {maximum}"
            )
    return spent
