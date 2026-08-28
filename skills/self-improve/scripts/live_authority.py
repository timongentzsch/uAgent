#!/usr/bin/env python3
"""Shared validation for live eval route authority."""

from __future__ import annotations

import hashlib
import json
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


def normalize_route_authority(model: str, declaration: Any) -> dict[str, Any]:
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
        if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= ceiling:
            raise AuthorityError(f"{model} {name} must be an integer in 1..{ceiling}")
        normalized_limits[name] = value
    return {"mode": "non-billable-cheap", "limits": normalized_limits}


def load_authority(path: Path | None, models: list[str]) -> dict[str, Any]:
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
    normalized = {model: normalize_route_authority(model, routes.get(model)) for model in models}
    return {
        "schema": SCHEMA,
        "routes": normalized,
        "sha256": hashlib.sha256(body).hexdigest(),
    }
