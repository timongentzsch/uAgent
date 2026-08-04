#!/usr/bin/env python3
"""Route-profile contract shared by the evaluator and its tests."""

from __future__ import annotations

import json
import os
import time
import urllib.parse
from pathlib import Path
from typing import Any

CERTIFICATION_COMPLIANCE = 1.0
PROFILE_LIFETIME_SECONDS = 30 * 24 * 60 * 60
ROUTE_PROFILE_SCHEMA = 3


def route_key(base_url: str, provider: str, model: str, effort: str) -> str:
    host = urllib.parse.urlparse(base_url).netloc.rsplit("@", 1)[-1]
    preferred = "" if provider == "default" else provider
    selected_effort = "" if effort == "default" else effort
    return f"{host.lower()}|{preferred}|{model}|{selected_effort}"


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int(fraction * len(ordered) + 0.999) - 1
    return ordered[max(0, min(len(ordered) - 1, index))]


def write_route_profiles(path: Path, summaries: list[dict[str, Any]]) -> None:
    existing: dict[str, Any] = {}
    if path.exists():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict) and loaded.get("schema") == ROUTE_PROFILE_SCHEMA:
                existing = loaded
        except (json.JSONDecodeError, OSError):
            pass
    routes = existing.get("routes")
    if not isinstance(routes, dict):
        routes = {}
    for route in sorted({str(item["route"]) for item in summaries}):
        route_results = [item for item in summaries if item["route"] == route]
        eligible = [
            item
            for item in route_results
            if item.get("outcome") not in {"error", "skipped", "cost_unavailable"}
        ]
        classes = sorted({str(item.get("scenario_class") or item["scenario"]) for item in eligible})
        scenario_samples = {
            scenario: sum(
                (item.get("scenario_class") or item["scenario"]) == scenario for item in eligible
            )
            for scenario in classes
        }
        pass_rate = sum(bool(item.get("passed")) for item in eligible) / max(len(eligible), 1)
        contradicted = any(not item.get("passed") for item in route_results)
        if contradicted or not classes or pass_rate < CERTIFICATION_COMPLIANCE:
            routes.pop(route, None)
            continue
        samples = [item for item in eligible if item.get("passed")]
        profile = dict(routes.get(route) or {})
        scenarios = sorted({str(item["scenario"]) for item in samples})
        profile.update(
            {
                "certified_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "certified_at_unix": int(time.time()),
                "samples": len(samples),
                "passing_samples": len(samples),
                "pass_rate": pass_rate,
                "certified": True,
                "memory_enabled": all(bool(item.get("memory_enabled", True)) for item in samples),
                "memory_generate": all(bool(item.get("memory_generate", True)) for item in samples),
                "scenario_classes": classes,
                "scenario_samples": scenario_samples,
                "p95_latency_ms": round(
                    percentile([float(item["elapsed_seconds"]) * 1000 for item in samples], 0.95),
                    3,
                ),
                "cost_per_scenario": {
                    scenario: round(
                        sum(
                            float(item["reported_cost"])
                            for item in samples
                            if item["scenario"] == scenario
                        )
                        / sum(item["scenario"] == scenario for item in samples),
                        8,
                    )
                    for scenario in scenarios
                },
                "parallel_hint_support": all(
                    bool(item["capabilities"]["parallel_hint_support"]) for item in samples
                ),
            }
        )
        checkpoints = [item for item in samples if item["scenario"].startswith("checkpoint")]
        if checkpoints:
            profile["checkpoint_mode"] = (
                "apply"
                if all(bool(item["capabilities"]["checkpoint_apply"]) for item in checkpoints)
                else "shadow"
            )
        images = [item for item in samples if item["scenario"] == "image-input"]
        if images:
            profile["image_support"] = all(
                bool(item["capabilities"]["image_support"]) for item in images
            )
        profile["model"] = samples[0]["model"]
        profile["provider"] = samples[0]["provider"]
        profile["base_url"] = samples[0]["base_url"]
        host = urllib.parse.urlparse(profile["base_url"]).hostname or ""
        default_openrouter = host == "openrouter.ai" or host.endswith(".openrouter.ai")
        profile["openrouter_compatible"] = all(
            bool((sample.get("provenance") or {}).get("openrouter_compatible", default_openrouter))
            for sample in samples
        )
        resolved_effort = str((samples[0].get("provenance") or {}).get("resolved_effort") or "")
        profile["effort"] = resolved_effort or (
            "" if samples[0].get("effort") == "default" else str(samples[0].get("effort") or "")
        )
        profile.pop("invalidated_at_unix", None)
        profile.pop("invalidated_feature", None)
        routes[route] = profile

    candidates: dict[str, list[dict[str, Any]]] = {}
    for route, profile in routes.items():
        if not isinstance(profile, dict) or profile.get("invalidated_at_unix"):
            continue
        certified = int(profile.get("certified_at_unix") or 0)
        if certified <= 0 or int(time.time()) - certified > PROFILE_LIFETIME_SECONDS:
            continue
        if float(profile.get("pass_rate") or 0) < CERTIFICATION_COMPLIANCE:
            continue
        family = route_key(
            str(profile.get("base_url") or ""),
            str(profile.get("provider") or "default"),
            str(profile.get("model") or ""),
            "default",
        )
        costs = [float(cost) for cost in (profile.get("cost_per_scenario") or {}).values()]
        if not costs:
            continue
        mean_cost = sum(costs) / len(costs)
        candidates.setdefault(family, []).append(
            {
                "effort": str(profile.get("effort") or ""),
                "mean_cost": round(mean_cost, 8),
                "route": route,
                "scenario_samples": sorted((profile.get("scenario_samples") or {}).items()),
            }
        )

    recommendations: dict[str, Any] = {}
    for family, options in candidates.items():
        comparison_sets = {tuple(option["scenario_samples"]) for option in options}
        if len(comparison_sets) == 1:
            selected = min(options, key=lambda option: float(option["mean_cost"]))
            selected.pop("scenario_samples")
            recommendations[family] = selected
    payload = {
        "schema": ROUTE_PROFILE_SCHEMA,
        "routes": routes,
        "recommendations": recommendations,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix(".tmp")
    pending.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.chmod(pending, 0o600)
    pending.replace(path)
