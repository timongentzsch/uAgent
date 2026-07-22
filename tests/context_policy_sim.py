#!/usr/bin/env python3
"""Deterministic context-policy cost simulation.

This is deliberately a small economic model, not a model-quality benchmark.
Prices are normalized to the provider's ordinary input-token price. Automatic
prompt caching is assumed: a cache hit reads the reusable prefix and writes the
new suffix; a miss writes the whole prompt. Physical compaction is modeled as
one cached summarization request followed by a rewritten conversation prefix.
"""

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Provider:
    name: str
    cache_hit_rate: float
    cache_read_price: float = 0.10
    cache_write_price: float = 1.25
    output_price: float = 4.0


@dataclass(frozen=True)
class Scenario:
    name: str
    context: int
    reserve: int
    growth: tuple[int, ...]
    checkpoints: frozenset[int]
    provider: Provider
    durable_fraction: float = 0.16


@dataclass
class Result:
    policy: str
    cost: float = 0.0
    history: int = 0
    reusable: int = 0
    peak_prompt: int = 0
    compactions: int = 0
    logical_checkpoints: int = 0
    invalidated_cached: int = 0
    dropped_unsummarized: int = 0
    canonical: int = 0
    lost_canonical: int = 0
    overflows: int = 0
    recent: list[tuple[int, int]] = field(default_factory=list)


POLICIES = (
    "current-75",
    "append-90",
    "eager-checkpoints",
    "sliding-window",
    "cache-aware",
    "scratchpad-aware",
)
STABLE_PREFIX = 2_000
CHECKPOINT_NOTE = 180


def prompt_cost(provider: Provider, prompt: int, reusable: int) -> float:
    """Expected cost when `reusable` leading tokens have an eligible cache."""
    reusable = min(prompt, reusable)
    suffix = prompt - reusable
    hit = provider.cache_hit_rate
    hit_cost = reusable * provider.cache_read_price + suffix * provider.cache_write_price
    miss_cost = prompt * provider.cache_write_price
    return hit * hit_cost + (1.0 - hit) * miss_cost


def request(result: Result, scenario: Scenario) -> None:
    prompt = STABLE_PREFIX + result.history
    result.cost += prompt_cost(scenario.provider, prompt, result.reusable)
    result.peak_prompt = max(result.peak_prompt, prompt)
    result.reusable = prompt
    if prompt + scenario.reserve > scenario.context:
        result.overflows += 1


def summary_size(result: Result) -> int:
    # A fair comparison cannot let repeated summaries stay tiny forever:
    # durable task state accumulates and must survive every rewrite.
    return max(320, min(result.history, result.canonical + 120))


def compact(result: Result, scenario: Scenario) -> None:
    """Pay for a summarization request, then replace conversation history."""
    request(result, scenario)
    summary = summary_size(result)
    result.cost += summary * scenario.provider.output_price
    result.invalidated_cached += max(0, result.reusable - STABLE_PREFIX)
    result.history = summary
    result.reusable = STABLE_PREFIX
    result.compactions += 1
    result.recent.clear()


def slide(result: Result) -> None:
    """Keep a recent raw tail without paying for a model-generated summary."""
    keep = sum(tokens for tokens, _ in result.recent[-3:])
    keep_canonical = sum(durable for _, durable in result.recent[-3:])
    result.dropped_unsummarized += max(0, result.history - keep)
    result.lost_canonical += max(0, result.canonical - keep_canonical)
    result.canonical = keep_canonical
    result.invalidated_cached += max(0, result.reusable - STABLE_PREFIX)
    result.history = keep
    result.reusable = STABLE_PREFIX
    result.recent = result.recent[-3:]
    result.compactions += 1


def forecast_materialization(result: Result, scenario: Scenario, rounds: int) -> bool:
    """Estimate whether one rewrite pays back over a short remaining horizon."""
    average_growth = round(
        sum(tokens for tokens, _ in result.recent[-3:]) / min(3, len(result.recent))
    )

    keep_cost = 0.0
    keep_history = result.history
    keep_reusable = result.reusable
    for future in range(rounds):
        if future:
            keep_history += average_growth
        prompt = STABLE_PREFIX + keep_history
        keep_cost += prompt_cost(scenario.provider, prompt, keep_reusable)
        keep_reusable = prompt

    compact_prompt = STABLE_PREFIX + result.history
    materialize_cost = prompt_cost(scenario.provider, compact_prompt, result.reusable)
    summary = summary_size(result)
    materialize_cost += summary * scenario.provider.output_price
    materialized_history = summary
    materialized_reusable = STABLE_PREFIX
    for future in range(rounds):
        if future:
            materialized_history += average_growth
        prompt = STABLE_PREFIX + materialized_history
        materialize_cost += prompt_cost(scenario.provider, prompt, materialized_reusable)
        materialized_reusable = prompt

    # Avoid churn for marginal estimates: the predicted saving must exceed 10%.
    return materialize_cost < keep_cost * 0.90


def forecast_scratchpad_fold(result: Result, scenario: Scenario, rounds: int) -> bool:
    """Compare future prompts after a checkpoint emitted in the current turn.

    Unlike `/compact`, the checkpoint note is generated by the response already
    in flight, so neither branch pays another full-history request.
    """
    if rounds <= 0:
        return False
    average_growth = round(
        sum(tokens for tokens, _ in result.recent[-3:]) / min(3, len(result.recent))
    )

    keep_cost = CHECKPOINT_NOTE * scenario.provider.output_price
    keep_history = result.history + CHECKPOINT_NOTE
    keep_reusable = result.reusable
    folded = summary_size(result)
    fold_cost = folded * scenario.provider.output_price
    fold_history = folded
    fold_reusable = STABLE_PREFIX

    for future in range(rounds):
        if future:
            keep_history += average_growth
            fold_history += average_growth
        keep_prompt = STABLE_PREFIX + keep_history
        fold_prompt = STABLE_PREFIX + fold_history
        keep_cost += prompt_cost(scenario.provider, keep_prompt, keep_reusable)
        fold_cost += prompt_cost(scenario.provider, fold_prompt, fold_reusable)
        keep_reusable = keep_prompt
        fold_reusable = fold_prompt

    return fold_cost < keep_cost * 0.90


def fold_scratchpad(result: Result, scenario: Scenario) -> None:
    """Materialize a note already generated in the current normal response."""
    summary = summary_size(result)
    result.cost += summary * scenario.provider.output_price
    result.invalidated_cached += max(0, result.reusable - STABLE_PREFIX)
    result.history = summary
    result.reusable = STABLE_PREFIX
    result.compactions += 1
    result.logical_checkpoints += 1
    result.recent.clear()


def simulate(scenario: Scenario, policy: str, scratchpad_trigger: float = 0.65) -> Result:
    result = Result(policy=policy)
    for turn, growth in enumerate(scenario.growth, 1):
        durable = round(growth * scenario.durable_fraction)
        result.history += growth
        result.canonical += durable
        result.recent.append((growth, durable))

        checkpoint = turn in scenario.checkpoints
        if policy == "scratchpad-aware":
            # The model first sees the full append-only turn. Its response can
            # then choose how the scratchpad will appear in the *next* prompt.
            request(result, scenario)
            projected = STABLE_PREFIX + result.history + scenario.reserve
            occupancy = projected / scenario.context
            rounds = min(6, len(scenario.growth) - turn)
            assessment_due = checkpoint and occupancy >= scratchpad_trigger
            if rounds > 0 and (
                occupancy >= 0.85
                or (assessment_due and forecast_scratchpad_fold(result, scenario, rounds))
            ):
                fold_scratchpad(result, scenario)
            elif assessment_due:
                result.cost += CHECKPOINT_NOTE * scenario.provider.output_price
                result.history += CHECKPOINT_NOTE
                result.logical_checkpoints += 1
            continue

        if policy == "cache-aware" and checkpoint:
            result.history += CHECKPOINT_NOTE
            recent_tokens, recent_durable = result.recent[-1]
            result.recent[-1] = (
                recent_tokens + CHECKPOINT_NOTE,
                recent_durable,
            )
            result.logical_checkpoints += 1

        projected = STABLE_PREFIX + result.history + scenario.reserve
        occupancy = projected / scenario.context
        if policy == "current-75" and occupancy >= 0.75:
            compact(result, scenario)
        elif policy == "append-90" and occupancy >= 0.90:
            compact(result, scenario)
        elif policy == "eager-checkpoints" and checkpoint:
            compact(result, scenario)
        elif policy == "sliding-window" and occupancy >= 0.75:
            slide(result)
        elif policy == "cache-aware":
            rounds = min(6, len(scenario.growth) - turn + 1)
            if occupancy >= 0.85 or (
                checkpoint and forecast_materialization(result, scenario, rounds)
            ):
                compact(result, scenario)

        request(result, scenario)
    return result


def growth(rounds: int, base: int, spike_every: int, spike: int) -> tuple[int, ...]:
    return tuple(base + (spike if turn % spike_every == 0 else 0) for turn in range(1, rounds + 1))


def scenarios() -> tuple[Scenario, ...]:
    warm = Provider("warm-cache", 0.90)
    weak = Provider("weak-cache", 0.35)
    no_write_premium = Provider("legacy-cache", 0.80, cache_read_price=0.25, cache_write_price=1.0)
    # Best-case bound calibrated to the 2026-07-20 live StreamLake probe:
    # one warm append reused 20,480/20,944 prompt tokens, cache reads were
    # priced at 0.2x, and no separate cache-write premium was advertised.
    # A single hit is not a measured long-run hit probability, so this scenario
    # deliberately models an ideal sticky route rather than claiming 97.8%.
    measured_warm = Provider(
        "measured-warm-bound", 1.0, cache_read_price=0.20, cache_write_price=1.0
    )
    return (
        Scenario(
            "short",
            64_000,
            8_000,
            growth(6, 600, 5, 500),
            frozenset({5}),
            warm,
        ),
        Scenario(
            "long-warm",
            64_000,
            8_000,
            growth(36, 1_400, 6, 4_000),
            frozenset(range(6, 37, 6)),
            warm,
        ),
        Scenario(
            "long-weak",
            64_000,
            8_000,
            growth(36, 1_400, 6, 4_000),
            frozenset(range(6, 37, 6)),
            weak,
        ),
        Scenario(
            "correction-heavy",
            48_000,
            6_000,
            growth(30, 1_100, 5, 3_000),
            frozenset(range(5, 31, 5)),
            warm,
        ),
        Scenario(
            "legacy-pricing",
            64_000,
            8_000,
            growth(36, 1_400, 6, 4_000),
            frozenset(range(6, 37, 6)),
            no_write_premium,
        ),
        Scenario(
            "measured-warm-bound",
            64_000,
            8_000,
            growth(36, 1_400, 6, 4_000),
            frozenset(range(6, 37, 6)),
            measured_warm,
        ),
    )


def print_results(all_results: dict[str, dict[str, Result]], cases: tuple[Scenario, ...]) -> None:
    print("scenario           policy              cost idx  comp  peak  invalidated  lost facts")
    for scenario in cases:
        results = all_results[scenario.name]
        baseline = results["current-75"].cost
        for policy in POLICIES:
            result = results[policy]
            print(
                f"{scenario.name:18} {policy:19} "
                f"{result.cost / baseline * 100:7.1f} "
                f"{result.compactions:5d} "
                f"{result.peak_prompt / scenario.context * 100:5.1f}% "
                f"{result.invalidated_cached / 1000:9.1f}k "
                f"{result.lost_canonical / 1000:7.1f}k"
            )
        print()


def sensitivity() -> None:
    print("cache-hit sensitivity (long workload; current-75 = 100)")
    print("hit rate  append-90  eager-checkpoints  cache-aware")
    workload = growth(36, 1_400, 6, 4_000)
    points = frozenset(range(6, 37, 6))
    for hit in (0.0, 0.25, 0.35, 0.5, 0.75, 0.9, 1.0):
        scenario = Scenario(
            "sensitivity",
            64_000,
            8_000,
            workload,
            points,
            Provider("sweep", hit),
        )
        results = {policy: simulate(scenario, policy) for policy in POLICIES}
        baseline = results["current-75"].cost
        print(
            f"{hit:7.0%} "
            f"{results['append-90'].cost / baseline * 100:10.1f} "
            f"{results['eager-checkpoints'].cost / baseline * 100:18.1f} "
            f"{results['cache-aware'].cost / baseline * 100:12.1f}"
        )


def durability_sensitivity() -> None:
    print("durable-state sensitivity (long warm-cache workload; current-75 = 100)")
    print("durable share  eager-checkpoints  cache-aware")
    base = scenarios()[1]
    for durable in (0.08, 0.16, 0.30):
        scenario = Scenario(
            base.name,
            base.context,
            base.reserve,
            base.growth,
            base.checkpoints,
            base.provider,
            durable,
        )
        results = {policy: simulate(scenario, policy) for policy in POLICIES}
        baseline = results["current-75"].cost
        print(
            f"{durable:12.0%} "
            f"{results['eager-checkpoints'].cost / baseline * 100:18.1f} "
            f"{results['cache-aware'].cost / baseline * 100:12.1f}"
        )
        assert results["cache-aware"].cost < results["current-75"].cost
        assert results["cache-aware"].cost < results["eager-checkpoints"].cost


def scratchpad_trigger_sensitivity() -> None:
    print("scratchpad trigger sensitivity (cost/peak; current-75 cost = 100)")
    print("trigger  long-warm       long-weak       correction-heavy")
    selected = (scenarios()[1], scenarios()[2], scenarios()[3])
    for trigger in (0.40, 0.50, 0.65, 0.75):
        cells = []
        for scenario in selected:
            baseline = simulate(scenario, "current-75").cost
            result = simulate(scenario, "scratchpad-aware", trigger)
            cells.append(
                f"{result.cost / baseline * 100:5.1f}/{result.peak_prompt / scenario.context:4.0%}"
            )
        print(f"{trigger:7.0%}  " + "    ".join(cells))


def validate(all_results: dict[str, dict[str, Result]]) -> None:
    for scenario_results in all_results.values():
        for result in scenario_results.values():
            assert result.overflows == 0, (result.policy, result.overflows)
        assert scenario_results["scratchpad-aware"].lost_canonical == 0

    short = all_results["short"]
    assert short["current-75"].compactions == 0
    assert short["cache-aware"].compactions == 0

    warm = all_results["long-warm"]
    assert warm["eager-checkpoints"].compactions > warm["cache-aware"].compactions
    assert warm["eager-checkpoints"].cost > warm["cache-aware"].cost
    assert warm["eager-checkpoints"].invalidated_cached > warm["cache-aware"].invalidated_cached

    weak = all_results["long-weak"]
    assert weak["cache-aware"].cost < weak["append-90"].cost

    measured = all_results["measured-warm-bound"]
    assert measured["scratchpad-aware"].lost_canonical == 0
    assert measured["scratchpad-aware"].cost < measured["eager-checkpoints"].cost

    for scenario in (
        "long-warm",
        "long-weak",
        "correction-heavy",
        "legacy-pricing",
        "measured-warm-bound",
    ):
        assert all_results[scenario]["sliding-window"].dropped_unsummarized > 0


def main() -> None:
    cases = scenarios()
    all_results = {
        scenario.name: {policy: simulate(scenario, policy) for policy in POLICIES}
        for scenario in cases
    }
    validate(all_results)
    print_results(all_results, cases)
    sensitivity()
    durability_sensitivity()
    scratchpad_trigger_sensitivity()


if __name__ == "__main__":
    main()
