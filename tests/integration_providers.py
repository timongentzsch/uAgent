import json
import os
import time

from integration_support import (
    OPENROUTER_SUPPORTS,
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    budget,
    event,
    function_names,
    provider_env,
    run,
    run_dialog,
    run_pty,
    save_settings,
    saved_settings,
    tool_call,
    tool_results,
    two_route_providers,
    write_json_response,
    write_sse_sequence,
)


def test_streamed_search_citations(root, home, *, binary):
    citation = {
        "type": "url_citation",
        "url_citation": {
            "url": "https://example.com/source",
            "title": "Source",
            "content": "search snippet",
        },
    }
    citation_without_usage = {
        "type": "url_citation",
        "url_citation": {
            "url": "https://example.com/legacy",
            "title": "Legacy source",
            "content": "legacy snippet",
        },
    }
    with Server(
        [
            {
                "choices": [
                    {
                        "delta": {"content": "grounded", "annotations": [citation]},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 2,
                    "completion_tokens": 1,
                    "server_tool_use_details": {"web_search_requests": 1},
                },
            },
            {
                "choices": [
                    {
                        "delta": {
                            "content": "legacy",
                            "annotations": [citation_without_usage],
                        },
                        "finish_reason": "stop",
                    }
                ],
                "usage": {"prompt_tokens": 2, "completion_tokens": 1},
            },
        ]
    ) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "probe\nagain\n/q\n",
            "--verbosity",
            "full",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("grounded\n  ← web_search" in result.stdout, result.stdout)
        assert_true("web_search ×1 · 1 source" in result.stdout, result.stdout)
        assert_true("legacy\n  ← 1 source" in result.stdout, result.stdout)
        assert_true("Sources:" in result.stdout, result.stdout)
        assert_true("https://example.com/source" in result.stdout, result.stdout)
        assert_true("legacy snippet" in result.stdout, result.stdout)


# Auto approval asks a decision model, and what it cannot decide goes to a person.
DECISION_BARS = {"judge-model": {"allow": 0.6, "deny": 0.95, "veto": 0.7}}


def decision_providers(url, supports=("decisions",), **options):
    """A provider that declares decision models, served by a mock at `url`."""
    return {
        "judge": {
            "base_url": url,
            "api_key": "review-key",
            "supports": list(supports),
            "options": {
                "decisions_url": url + "/decisions",
                "decisions_model": "judge-model",
                "decision_bars": DECISION_BARS,
                **options,
            },
        }
    }


def decision_answers(allow, deny=0.0, **reasons):
    names = ["destructive", "beyond_request", "sends_data_off_machine", "touches_secrets"]
    answers = {
        "permission": {
            "type": "choice",
            "choice": "allow" if allow >= deny else "deny",
            "confidence": max(allow, deny),
            "probabilities": {"allow": allow, "ask": 1 - allow - deny, "deny": deny},
        }
    }
    for name in names:
        answers[name] = {"type": "noul", "noul": reasons.get(name, 0.0)}
    return {"answers": answers, "usage": {"input_tokens": 7, "output_tokens": 1, "cost": 0.0001}}


def auto_review_run(
    root, home, binary, calls, reviewer, *, finish=None, environment=None, **provider
):
    """Runs one headless turn in Auto mode whose tool calls the mock reviewer
    judges; returns the envelope, the model's last view of a tool result and
    the reviewer's requests."""
    seen = {}

    def last(_, body):
        seen["tool"] = tool_results(body["messages"])[-1]
        return event({"content": "auto-review-done"})

    with Server([reviewer]) as permission_server:
        with Server([*calls, finish or last]) as model_server:
            env = provider_env(
                home,
                model_server.url,
                decision_providers(permission_server.url, **provider),
                model="test",
            )
            env["UAGENT_APPROVAL"] = "auto"
            env.update(environment or {})
            result = run(root, env, "--json", "-p", "go", binary=binary)
            envelope = json.loads(result.stdout)
            assert_true(result.returncode == 0, (result.stderr, envelope))
            assert_true(envelope["answer"] == "auto-review-done", envelope)
            return envelope, seen.get("tool", ""), list(permission_server.requests)


def write_call(path, content="x", call_id="call-1"):
    return tool_call("write_file", {"path": str(path), "content": content}, call_id=call_id)


def test_auto_permission_reviewer_allows_and_accounts(root, home, *, binary):
    target = root / "reviewed.txt"

    def reviewer(handler, body):
        assert_true(body["model"] == "judge-model", body)
        assert_true(body["state"]["tool"] == "write_file", body)
        assert_true(body["state"]["user_request"] == "go", body)
        assert_true(
            set(body["questions"])
            == {
                "permission",
                "destructive",
                "beyond_request",
                "sends_data_off_machine",
                "touches_secrets",
            },
            body,
        )
        assert_true(
            set(body["questions"]["permission"]["criteria"]) == {"allow", "ask", "deny"}, body
        )
        # This endpoint declares neither provider preferences nor sessions.
        assert_true("provider" not in body and "session_id" not in body, body)
        write_json_response(handler, decision_answers(0.9, 0.0, destructive=0.1))

    envelope, tool, requests = auto_review_run(
        root, home, binary, [write_call(target, "approved")], reviewer
    )
    assert_true("wrote" in tool, tool)
    assert_true(target.read_text() == "approved", target)
    assert_true(len(requests) == 1, requests)
    assert_true(requests[0][0]["Authorization"] == "Bearer review-key", requests[0][0])
    assert_true(envelope["usage"]["input"] >= 7, envelope)
    assert_true(envelope["usage"]["cost_reported"], envelope)
    assert_true(any("permission_review" in route for route in envelope["routes"]), envelope)


def test_auto_permission_reviewer_bars(root, home, *, binary):
    target = root / "bars.txt"

    # A risk reason at its bar forces the question, however sure the allow is;
    # with nobody to ask, the call is refused and the model is told why.
    def sure_but_risky(handler, _):
        write_json_response(handler, decision_answers(0.99, 0.0, sends_data_off_machine=0.7))

    def unsure(handler, _):
        write_json_response(handler, decision_answers(0.59))

    for reviewer in (sure_but_risky, unsure):
        _, tool, _ = auto_review_run(root, home, binary, [write_call(target)], reviewer)
        assert_true("could not decide" in tool and "user denied" not in tool, tool)
        assert_true(not target.exists(), target)

    # The deny bar refuses outright, and says the review did.
    def denies(handler, _):
        write_json_response(handler, decision_answers(0.0, 0.95))

    _, tool, _ = auto_review_run(root, home, binary, [write_call(target)], denies)
    assert_true("refused this action" in tool, tool)
    assert_true(not target.exists(), target)

    # The bars are data of the endpoint: the same 0.9 that the ordinary bars
    # allow asks under a row that wants 0.99.
    def fairly_sure(handler, _):
        write_json_response(handler, decision_answers(0.9))

    def sure(handler, _):
        write_json_response(handler, decision_answers(0.99))

    strict = {"judge-model": {"allow": 0.99, "deny": 0.95, "veto": 0.7}}
    _, tool, _ = auto_review_run(
        root, home, binary, [write_call(target)], fairly_sure, decision_bars=strict
    )
    assert_true("could not decide" in tool and not target.exists(), tool)
    auto_review_run(root, home, binary, [write_call(target)], sure, decision_bars=strict)
    assert_true(target.read_text() == "x", target)


def test_auto_permission_card_shows_the_reasons(root, home, *, binary):
    # What the person is asked comes with the reasons that made it a question,
    # by probability, and the bar they fell on.
    def reviewer(handler, _):
        write_json_response(
            handler,
            decision_answers(0.99, 0.0, sends_data_off_machine=0.7, beyond_request=0.55),
        )

    with Server([reviewer]) as permission_server:
        with Server([write_call(root / "card.txt"), event({"content": "card-ok"})]) as model:
            env = provider_env(
                home, model.url, decision_providers(permission_server.url), model="test"
            )
            env["UAGENT_APPROVAL"] = "auto"
            code, output = run_pty(
                root,
                env,
                [(b"go\n", b"approval needed:"), (b"n\n", b"card-ok"), (b"/q\n", None)],
                args=("--plain",),
                startup_marker=b"",
                binary=binary,
            )
    assert_true(code == 0, output[-2000:])
    for text in (
        b"Risk: makes changes - beyond what you asked 55% - sends data off this machine 70%",
        b"Auto review sends data off this machine 70% reaches the 70% that asks",
    ):
        assert_true(text in output, output[-2000:])
    assert_true(not (root / "card.txt").exists(), output[-2000:])


def test_auto_permission_reviewer_judges_against_the_first_request_too(root, home, *, binary):
    """A later message narrows what was asked; the reviewer reads both, so an
    action the first request covers is not judged against the note alone."""
    target = root / "reviewed.txt"
    seen = []

    def reviewer(handler, body):
        seen.append(body["state"]["user_request"])
        write_json_response(handler, decision_answers(0.99))

    with Server([reviewer]) as permission_server:
        with Server(
            [
                event({"content": "noted"}),
                tool_call("write_file", {"path": str(target), "content": "approved"}),
                event({"content": "written"}),
            ]
        ) as model_server:
            env = provider_env(
                home, model_server.url, decision_providers(permission_server.url), model="test"
            )
            env["UAGENT_APPROVAL"] = "auto"
            result = run_dialog(
                root, env, "write reviewed.txt\nalso keep it short\n/q\n", binary=binary
            )
            assert_true(result.returncode == 0, result.stderr)
            assert_true(target.read_text() == "approved", result.stdout)
            assert_true(len(seen) == 1, seen)
            # The latest message leads, so a cut for length never loses it.
            assert_true(
                0 <= seen[0].find("also keep it short") < seen[0].find("write reviewed.txt"), seen
            )


def test_auto_permission_reviewer_errors_in_words(root, home, *, binary):
    target = root / "must-not-exist.txt"
    for status, message, words in [
        (402, "insufficient credits", "has no credit"),
        (401, "bad key", "API key was refused"),
        (413, "too big", "too large"),
        (404, "No endpoints found matching your data policy", "data policy"),
    ]:

        def failing(handler, _, status=status, message=message):
            write_json_response(
                handler, {"error": {"code": status, "message": message}}, status=status
            )

        _, tool, requests = auto_review_run(root, home, binary, [write_call(target)], failing)
        assert_true(words in tool, (status, tool))
        assert_true(len(requests) == 1, (status, requests))
        assert_true(not target.exists(), target)


def test_auto_permission_reviewer_needs_an_address(root, home, *, binary):
    target = root / "no-address.txt"

    def never(*_):
        raise AssertionError("no address, no request")

    _, tool, requests = auto_review_run(
        root, home, binary, [write_call(target)], never, decisions_url="decide"
    )
    assert_true("not an address" in tool and requests == [], (tool, requests))
    assert_true(not target.exists(), target)


def test_auto_permission_reviewer_privacy_and_session(root, home, *, binary):
    private = root / "private.txt"
    open_ = root / "open.txt"
    declared = ("decisions", "provider_routing", "session_passthrough")

    def reviewer(handler, body):
        assert_true(body["provider"] == {"zdr": True, "data_collection": "deny"}, body)
        # A hash of the session, never its id.
        assert_true(len(body["session_id"]) == 16, body)
        write_json_response(handler, decision_answers(0.9))

    auto_review_run(root, home, binary, [write_call(private)], reviewer, supports=declared)

    # The endpoint's option turns the retention filter off; the session hash stays.
    def unfiltered(handler, body):
        assert_true("provider" not in body and len(body["session_id"]) == 16, body)
        write_json_response(handler, decision_answers(0.9))

    auto_review_run(
        root,
        home,
        binary,
        [write_call(open_)],
        unfiltered,
        supports=declared,
        decisions_private=False,
    )
    assert_true(private.exists() and open_.exists(), (private, open_))


def test_auto_permission_reviewer_session_memory(root, home, *, binary):
    # An identical call allowed automatically is not reviewed again.
    target = root / "twice.txt"

    def reviewer(handler, _):
        write_json_response(handler, decision_answers(0.9))

    _, _, requests = auto_review_run(
        root,
        home,
        binary,
        [write_call(target, "again", "first"), write_call(target, "again", "second")],
        reviewer,
    )
    assert_true(len(requests) == 1, requests)
    assert_true(target.read_text() == "again", target)


def test_auto_permission_never_reviews_outside_the_folder(root, home, *, binary):
    outside = root.parent / (root.name + "-outside.txt")

    def must_not_be_asked(*_):
        raise AssertionError("the reviewer was asked about a path outside the folder")

    _, tool, requests = auto_review_run(
        root, home, binary, [write_call(outside)], must_not_be_asked
    )
    assert_true(requests == [], requests)
    assert_true(not outside.exists(), outside)
    assert_true("denied" in tool, tool)


def test_auto_permission_never_reviews_commands_without_a_sandbox(root, home, *, binary):
    def must_not_be_asked(*_):
        raise AssertionError("the reviewer was asked about a command with no sandbox")

    for name, arguments in [
        ("run", {"command": "echo hi"}),
        ("scratch", {"path": "t.py", "code": "print(1)"}),
    ]:
        _, tool, requests = auto_review_run(
            root,
            home,
            binary,
            [tool_call(name, arguments)],
            must_not_be_asked,
            environment={"UAGENT_SANDBOX": "off"},
        )
        assert_true(requests == [], (name, requests))
        assert_true("denied" in tool, (name, tool))


def test_auto_permission_without_decision_endpoint_asks(root, home, *, binary):
    target = root / "must-not-exist.txt"

    def finish(_, body):
        # The model hears who refused, and that it was not the user.
        refused = tool_results(body["messages"])[-1]
        assert_true("automatic permission review" in refused, refused)
        assert_true("serves decision models" in refused, refused)
        assert_true("user denied" not in refused, refused)
        return event({"content": "review-failed-closed"})

    with Server([write_call(target, "not approved"), finish]) as model_server:
        env = base_env(home, model_server.url)
        env.pop("OPENROUTER_API_KEY", None)
        env["UAGENT_APPROVAL"] = "auto"
        result = run(root, env, "--json", "-p", "write it", binary=binary)
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, (result.stderr, envelope))
        assert_true(envelope["answer"] == "review-failed-closed", envelope)
        assert_true(not target.exists(), target)


def test_openrouter_named_search_contract_and_errors(root, home, *, binary):
    citation = {
        "type": "url_citation",
        "url_citation": {"url": "https://example.com/current", "title": "Current"},
    }

    def successful_search(_, body):
        assert_true(body["model"] == "search-model", body)
        return {
            "choices": [
                {
                    "message": {"content": "grounded answer", "annotations": [citation]},
                    "finish_reason": "stop",
                }
            ],
            "usage": {
                "prompt_tokens": 5,
                "completion_tokens": 2,
                "server_tool_use_details": {"web_search_requests": 2},
            },
        }

    def rejected_search(handler, _):
        write_json_response(handler, {"error": {"message": "rate limited"}}, status=429)

    def ask_again(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("grounded answer" in result, result)
        assert_true("https://example.com/current" in result, result)
        return tool_call("web_search", {"queries": ["failure probe"]}, call_id="search-2")

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("web_search HTTP 429: rate limited" in result, result)
        return event({"content": "search-contract-ok"})

    with Server([successful_search, rejected_search], repeat_last=True) as search_server:
        with Server(
            [
                tool_call("web_search", {"queries": ["current fact"]}),
                ask_again,
                finish,
            ]
        ) as model_server:
            env = base_env(home, model_server.url)
            env.update(
                {
                    "UAGENT_INTERNAL_FEATURES": ",".join(OPENROUTER_SUPPORTS),
                    "UAGENT_WEB_SEARCH_BACKEND": "auto",
                    "UAGENT_PROVIDERS": json.dumps(
                        {
                            "search": {
                                "base_url": search_server.url,
                                "api_key": "search-key",
                                "supports": OPENROUTER_SUPPORTS,
                            }
                        }
                    ),
                    "UAGENT_WEB_SEARCH_MODEL": "search/search-model",
                }
            )
            result = run(root, env, "--yolo", "--json", "-p", "search", binary=binary)
            envelope = json.loads(result.stdout)
            assert_true(result.returncode == 0, (result.stderr, envelope))
            assert_true(envelope["answer"] == "search-contract-ok", envelope)
            assert_true(envelope["usage"]["web_searches"] == 2, envelope)
            assert_true(
                all(
                    function_names(body) >= {"web_search"}
                    and all(tool.get("type") == "function" for tool in body.get("tools", []))
                    for _, body in model_server.requests
                ),
                model_server.requests,
            )


def test_web_search_retries_respect_turn_deadline(root, home, *, binary):
    def busy(handler, _):
        data = json.dumps({"error": {"message": "fixture busy"}}).encode()
        handler.send_response(503)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Retry-After", "60")
        handler.end_headers()
        handler.wfile.write(data)

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("fixture busy" in result, result)
        return event({"content": "search-budget-ok"})

    with Server([busy]) as search_server:
        with Server(
            [tool_call("web_search", {"queries": ["deadline probe"]}), finish]
        ) as model_server:
            env = base_env(home, model_server.url)
            env.update(
                {
                    "UAGENT_INTERNAL_FEATURES": ",".join(OPENROUTER_SUPPORTS),
                    "UAGENT_WEB_SEARCH_BACKEND": "auto",
                    "UAGENT_PROVIDERS": json.dumps(
                        {
                            "search": {
                                "base_url": search_server.url,
                                "api_key": "search-key",
                                "supports": OPENROUTER_SUPPORTS,
                            }
                        }
                    ),
                    "UAGENT_WEB_SEARCH_MODEL": "search/search-model",
                    "UAGENT_MAX_TURN_SECONDS": str(int(budget(2))),
                }
            )
            started = time.monotonic()
            result = run(root, env, "--yolo", "-p", "search", timeout=8, binary=binary)
            assert_true(result.returncode == 0, (result.stdout, result.stderr))
            assert_true(result.stdout.strip() == "search-budget-ok", result.stdout)
            assert_true(len(search_server.requests) == 1, search_server.requests)
            assert_true(time.monotonic() - started < budget(6), result.stderr)


def test_web_search_slow_response_respects_turn_deadline(root, home, *, binary):
    def retry(handler, _):
        write_json_response(handler, {"error": {"message": "retry once"}}, status=503)

    def busy(handler, _):
        time.sleep(budget(8))
        try:
            write_json_response(handler, {"choices": []})
        except (BrokenPipeError, ConnectionResetError):
            pass

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("timeout" in result.lower() or "deadline" in result.lower(), result)
        return event({"content": "search-budget-ok"})

    with Server([retry, busy], repeat_last=True) as search_server:
        with Server(
            [tool_call("web_search", {"queries": ["deadline probe"]}), finish]
        ) as model_server:
            env = base_env(home, model_server.url)
            env.update(
                {
                    "UAGENT_INTERNAL_FEATURES": ",".join(OPENROUTER_SUPPORTS),
                    "UAGENT_WEB_SEARCH_BACKEND": "auto",
                    "UAGENT_PROVIDERS": json.dumps(
                        {
                            "search": {
                                "base_url": search_server.url,
                                "api_key": "search-key",
                                "supports": OPENROUTER_SUPPORTS,
                            }
                        }
                    ),
                    "UAGENT_WEB_SEARCH_MODEL": "search/search-model",
                    "UAGENT_MAX_TURN_SECONDS": str(int(budget(4))),
                }
            )
            started = time.monotonic()
            result = run(root, env, "--yolo", "-p", "search", timeout=8, binary=binary)
            assert_true(result.returncode == 0, (result.stdout, result.stderr))
            assert_true(result.stdout.strip() == "search-budget-ok", result.stdout)
            assert_true(len(search_server.requests) == 2, search_server.requests)
            assert_true(time.monotonic() - started < budget(6), result.stderr)


def test_openrouter_reasoning_details_survive_tool_step(root, home, *, binary):
    details = [
        {
            "type": "reasoning.text",
            "index": 0,
            "text": "provider thought",
            "signature": "opaque-signature",
        }
    ]

    def verify_tool_step(_, body):
        assistant = next(
            message
            for message in body["messages"]
            if message.get("role") == "assistant" and message.get("tool_calls")
        )
        return event(
            {
                "content": (
                    "openrouter-replay-ok"
                    if assistant.get("reasoning_details") == details
                    else "openrouter-replay-bad"
                )
            }
        )

    first = event(
        {
            "reasoning_details": details,
            "tool_calls": [
                {
                    "index": 0,
                    "id": "reasoning-call",
                    "function": {
                        "name": "read_path",
                        "arguments": json.dumps({"path": "."}),
                    },
                }
            ],
        },
        finish="tool_calls",
    )
    with Server([first, verify_tool_step]) as server:
        env = base_env(home, server.url)
        env["UAGENT_INTERNAL_FEATURES"] = ",".join(OPENROUTER_SUPPORTS)
        result = run(root, env, "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "openrouter-replay-ok", result.stdout)


def test_provider_context_overflow_compacts_once(root, home, *, binary):
    prompt = "preserve-overflow-goal " + ("evidence " * 2500)

    def reject(handler, _):
        write_json_response(
            handler,
            {
                "error": {
                    "type": "invalid_request_error",
                    "code": "context_length_exceeded",
                    "message": "input exceeds the context window",
                    "param": "input",
                }
            },
            status=400,
        )

    def compact(_, body):
        serialized = json.dumps(body["messages"])
        assert_true("Summarize the bounded transcript" in serialized, serialized)
        assert_true("preserve-overflow-goal" in serialized, serialized)
        assert_true("tools" not in body, body)
        assert_true(len(serialized) < 300_000, len(serialized))
        return event({"content": "preserved goal and current state"})

    def recovered(_, body):
        serialized = json.dumps(body["messages"])
        assert_true("model-generated context summary" in serialized, serialized)
        assert_true("preserve-overflow-goal" in serialized, serialized)
        assert_true("harness continuation after context compaction" not in serialized, serialized)
        return event({"content": "context-recovery-ok"})

    with Server([reject, compact, recovered]) as server:
        result = run(root, base_env(home, server.url), "-p", prompt, timeout=15, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "context-recovery-ok", result.stdout)
        assert_true(len(server.requests) == 3, server.requests)

    def reject_413(handler, _):
        write_json_response(handler, {"error": "request too large"}, status=413)

    # If the bounded compaction request is also rejected, do not loop or replay
    # the original request. A later user turn can retry deliberately.
    with Server([reject_413]) as server:
        failed = run(root, base_env(home, server.url), "-p", prompt, timeout=15, binary=binary)
        assert_true(failed.returncode == 1, (failed.stdout, failed.stderr))
        assert_true(len(server.requests) == 2, server.requests)


def test_model_route_switch(root, home, *, binary):
    first = Server([event({"content": "wrong-provider"})])

    def switched(_, body):
        valid = (
            body.get("model") == "model-b"
            and body.get("reasoning_effort") == "high"
            and body.get("max_tokens") == 16000
        )
        return event({"content": "route-ok" if valid else "route-bad"})

    second = Server([switched])
    providers = two_route_providers(first.url, second.url)
    providers["second"]["models"]["fast"]["context"] = 8192
    try:
        env = provider_env(home, first.url, providers, "first/main")
        # Unset, the cap is omitted so the provider applies its own maximum;
        # the switched route still has to carry it.
        env["UAGENT_MAX_TOKENS"] = "16000"
        result = run_dialog(
            root, env, "/models all\n2\n/effort default\n/effort high\nprobe\n/q\n", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("first/main" in result.stdout and "second/fast" in result.stdout, result.stdout)
        assert_true("effort provider default" in result.stdout, result.stdout)
        assert_true("route-ok" in result.stdout, result.stdout)
        assert_true(not first.requests, first.requests)
        auth = second.requests[0][0].get("Authorization")
        assert_true(auth == "Bearer key-b", auth)
    finally:
        first.close()
        second.close()


def test_routing_suffix_in_a_model_id_is_sent_as_written(root, home, *, binary):
    router = Server([event({"content": "nitro-ok"}), event({"content": "floor-ok"})])
    providers = {
        "router": {
            "base_url": router.url,
            "api_key": "router-key",
            "supports": OPENROUTER_SUPPORTS,
            "models": {"main": "vendor/model:nitro"},
        },
    }
    try:
        env = provider_env(home, router.url, providers, "router/main")
        result = run_dialog(
            root,
            env,
            "one\n/model router/vendor/model:floor\ntwo\n/variant\n/q\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        for reply in ("nitro-ok", "floor-ok"):
            assert_true(reply in result.stdout, result.stdout)
        bodies = [body for _, body in router.requests]
        assert_true(
            [body.get("model") for body in bodies] == ["vendor/model:nitro", "vendor/model:floor"],
            bodies,
        )
        session_ids = [body.get("session_id") for body in bodies]
        assert_true(all(session_ids) and len(set(session_ids)) == 2, session_ids)
    finally:
        router.close()


def test_dynamic_provider_catalog_and_model(root, home, *, binary):
    active_catalog = {"data": [{"id": "active-live"}]}
    first = Server(
        [event({"content": "original-route-ok"})],
        get_response=active_catalog,
    )

    def switched(handler, body):
        valid = (
            body.get("model") == "gpt-live"
            and handler.headers.get("Authorization") == "Bearer key-b"
            and "reasoning_effort" not in body
        )
        return event({"content": "dynamic-route-ok" if valid else "dynamic-route-bad"})

    catalog = {"data": [{"id": "gpt-live", "context_length": 16384}]}
    second = Server([switched], get_response=catalog)
    providers = {
        "second": {
            "base_url": second.url,
            "api_key": "key-b",
            "context": 16384,
        }
    }
    try:
        env = provider_env(home, first.url, providers, "active-live")
        # A raw startup route may accept an explicit effort. A later catalog
        # route with no advertised effort support must replace, not inherit,
        # that state.
        env["UAGENT_REASONING_EFFORT"] = "high"
        catalog_result = run_dialog(root, env, "/models live\n\x1b\nprobe\n/q\n", binary=binary)
        assert_true(catalog_result.returncode == 0, catalog_result.stderr)
        assert_true(
            "searching all model catalogs for live" in catalog_result.stdout,
            catalog_result.stdout,
        )
        assert_true("second/gpt-live" in catalog_result.stdout, catalog_result.stdout)
        assert_true("active-live" in catalog_result.stdout, catalog_result.stdout)
        assert_true("keeping active-live" in catalog_result.stdout, catalog_result.stdout)
        assert_true("original-route-ok" in catalog_result.stdout, catalog_result.stdout)
        assert_true(len(first.requests) == 1, first.requests)

        wildcard = run_dialog(root, env, "/models second/*\n\x1b\n/q\n", binary=binary)
        assert_true(wildcard.returncode == 0, wildcard.stderr)
        assert_true("second/gpt-live" in wildcard.stdout, wildcard.stdout)

        selected = run_dialog(root, env, "/models gpt-live\n1\nprobe\n/q\n", binary=binary)
        assert_true(selected.returncode == 0, selected.stderr)
        assert_true("dynamic-route-ok" in selected.stdout, selected.stdout)
        assert_true(len(second.get_requests) == 3, second.get_requests)

        # A catalog route named as the configured model starts a fresh run.
        restart_env = provider_env(home, first.url, providers, "second/gpt-live")
        restarted = run(root, restart_env, "-p", "probe", binary=binary)
        assert_true(restarted.returncode == 0, restarted.stderr)
        assert_true(restarted.stdout.strip() == "dynamic-route-ok", restarted.stdout)
        assert_true(len(first.requests) == 1, first.requests)

        # Direct /model must discover the same window as the catalog picker.
        # Changing only the effort keeps it without another catalog request,
        # and the picker's queries left a catalogue that serves the first.
        providers["second"].pop("context")
        direct_env = provider_env(home, first.url, providers, "active-live")
        probes = len(second.get_requests)
        direct = run_dialog(
            root,
            direct_env,
            "/model second/gpt-live\n/model second/gpt-live:high\n/q\n",
            binary=binary,
        )
        assert_true(direct.returncode == 0, direct.stderr)
        assert_true(len(second.get_requests) == probes, second.get_requests)
    finally:
        first.close()
        second.close()


def test_configured_alias_keeps_catalog_efforts(root, home, *, binary):
    efforts = ["low", "medium", "high", "xhigh", "max"]
    catalog = {"data": [{"id": "gpt-live", "supported_reasoning_efforts": efforts}]}
    with Server([event({"content": "catalog-efforts-ok"})], get_response=catalog) as server:
        providers = {
            "local": {
                "base_url": server.url,
                "context": 16384,
                "models": {"main": {"id": "gpt-live"}},
            }
        }
        env = provider_env(home, server.url, providers, "local/main")
        result = run_dialog(
            root,
            env,
            "/effort none\n/effort minimal\n/model local/main:high\nprobe\n/q\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("catalog-efforts-ok" in result.stdout, result.stdout)
        assert_true(len(server.get_requests) == 1, server.get_requests)
        assert_true(server.requests[0][1]["reasoning_effort"] == "high", server.requests)
        assert_true(result.stdout.count("not supported") >= 2, result.stdout)


def test_model_choice_is_the_conversations_until_saved_for_all(root, home, *, binary):
    first = Server([event({"content": "configured-model-ok"})] * 3)

    def chosen(_, body):
        valid = body.get("model") == "model-b" and body.get("reasoning_effort") == "medium"
        return event({"content": "chosen-model-ok" if valid else "chosen-model-bad"})

    second = Server([chosen] * 2)
    providers = two_route_providers(first.url, second.url)
    providers["second"]["context"] = 8192
    try:
        choose_env = provider_env(home, first.url, providers, "first/main")
        picked = run_dialog(root, choose_env, "/model second/fast\n/config\n/q\n", binary=binary)
        assert_true(picked.returncode == 0, picked.stderr)
        # The choice is this conversation's: the setting at its scope, saved
        # nowhere else.
        assert_true("UAGENT_MODEL = second/model-b:medium" in picked.stdout, picked.stdout)
        assert_true("conversation" in picked.stdout, picked.stdout)
        assert_true(not saved_settings(home), "choice left the conversation")

        fresh_env = provider_env(home, first.url, providers)
        # A fresh run knows nothing of it: with no model configured it says so.
        fresh = run(root, fresh_env, "-p", "probe", binary=binary)
        assert_true(
            fresh.returncode != 0 and "no model set" in fresh.stdout + fresh.stderr,
            fresh.stdout + fresh.stderr,
        )
        assert_true(not second.requests, second.requests)

        # Saved for all conversations only when asked.
        kept = run_dialog(root, choose_env, "/model second/fast --default\n/q\n", binary=binary)
        assert_true("also the model of new conversations" in kept.stdout, kept.stdout)
        assert_true(
            saved_settings(home) == {"model": "second/model-b:medium"}, saved_settings(home)
        )
        started = run(root, fresh_env, "-p", "probe", binary=binary)
        assert_true(started.stdout.strip() == "chosen-model-ok", started.stdout + started.stderr)

        # The environment still decides over the saved setting.
        override_env = dict(fresh_env)
        override_env["UAGENT_MODEL"] = "first/main"
        overridden = run(root, override_env, "-p", "probe", binary=binary)
        assert_true(overridden.stdout.strip() == "configured-model-ok", overridden.stdout)
    finally:
        first.close()
        second.close()


def test_an_overloaded_provider_is_tried_again(root, home, *, binary):
    """A proxy reports its upstream's overload inside the stream, after the
    response was opened. Nothing was said yet, so the request is repeated and
    the turn goes on."""
    overloaded = (
        "Codex response failed: {'type': 'service_unavailable_error', 'code': "
        "'server_is_overloaded', 'headers': {'x-retry-metadata': 'NO_MORE_RETRY'}, "
        "'message': 'Our servers are currently overloaded. Please try again later.', "
        "'param': None}"
    )
    opened = {
        "type": "response.created",
        "sequence_number": 0,
        "response": {
            "id": "resp_1",
            "object": "response",
            "status": "in_progress",
            "model": "m",
            "output": [],
            "usage": None,
            "error": None,
            "incomplete_details": None,
        },
    }

    def busy(handler, _):
        write_sse_sequence(
            handler,
            [
                opened,
                {
                    "type": "error",
                    "sequence_number": 1,
                    "code": "upstream_error",
                    "message": overloaded,
                    "param": None,
                },
            ],
        )

    def answer(handler, _):
        write_sse_sequence(
            handler,
            [
                opened,
                {"type": "response.output_text.delta", "delta": "after-overload-ok"},
                {
                    "type": "response.completed",
                    "response": {
                        "status": "completed",
                        "usage": {"input_tokens": 8, "output_tokens": 3},
                    },
                },
            ],
        )

    with Server([busy, busy, answer]) as server:
        env = base_env(home, server.url)
        env["UAGENT_INTERNAL_WIRE_API"] = "responses"
        result = run(root, env, "--json", "-p", "go", timeout=60, binary=binary)
        envelope = json.loads(result.stdout.strip().splitlines()[-1])
        assert_true(envelope["answer"] == "after-overload-ok", envelope)
        assert_true(len(server.requests) == 3, len(server.requests))


def test_provider_responses_native_search_and_function_replay(root, home, *, binary):
    # The replayed function_call lists the workspace, so it needs an entry to
    # list: an empty directory reports itself as empty and never gets there.
    (root / "listed.txt").write_text("x\n", encoding="utf-8")

    def first(handler, body):
        assert_true(handler.path == "/v1/responses", handler.path)
        assert_true(handler.headers.get("Authorization") == "Bearer response-key", handler.headers)
        assert_true("input" in body and "messages" not in body, body)
        assert_true(body.get("include") == ["reasoning.encrypted_content"], body)
        hosted = [tool for tool in body["tools"] if tool.get("type") == "web_search"]
        functions = [tool for tool in body["tools"] if tool.get("type") == "function"]
        assert_true(len(hosted) == 1, body["tools"])
        assert_true(all(tool.get("name") != "web_search" for tool in functions), functions)
        write_sse_sequence(
            handler,
            [
                {
                    "type": "response.output_item.done",
                    "output_index": 0,
                    "item": {
                        "type": "reasoning",
                        "id": "reasoning-1",
                        "encrypted_content": "opaque-reasoning",
                    },
                },
                {
                    "type": "response.output_item.added",
                    "output_index": 1,
                    "item": {
                        "type": "function_call",
                        "call_id": "call-1",
                        "name": "read_path",
                        "arguments": "",
                    },
                },
                {
                    "type": "response.function_call_arguments.done",
                    "output_index": 1,
                    "call_id": "call-1",
                    "arguments": json.dumps({"path": "."}),
                },
                {
                    "type": "response.output_item.done",
                    "output_index": 1,
                    "item": {
                        "type": "function_call",
                        "call_id": "call-1",
                        "name": "read_path",
                        "arguments": json.dumps({"path": "."}),
                    },
                },
                {
                    "type": "response.completed",
                    "response": {
                        "status": "completed",
                        "usage": {"input_tokens": 4, "output_tokens": 2},
                    },
                },
            ],
        )

    def second(handler, body):
        assert_true(body.get("include") == ["reasoning.encrypted_content"], body)
        item_types = [item.get("type") for item in body["input"]]
        assert_true("reasoning" in item_types, body["input"])
        assert_true(item_types.count("function_call") == 1, body["input"])
        assert_true("function_call_output" in item_types, body["input"])
        reasoning = next(item for item in body["input"] if item.get("type") == "reasoning")
        assert_true(reasoning.get("encrypted_content") == "opaque-reasoning", reasoning)
        output = next(item for item in body["input"] if item.get("type") == "function_call_output")
        assert_true("entries" in output.get("output", ""), output)
        write_sse_sequence(
            handler,
            [
                {
                    "type": "response.output_item.done",
                    "output_index": 0,
                    "item": {
                        "type": "web_search_call",
                        "id": "search-1",
                        "status": "completed",
                    },
                },
                {"type": "response.output_text.delta", "delta": "responses-native-ok"},
                {
                    "type": "response.output_text.annotation.added",
                    "annotation": {
                        "type": "url_citation",
                        "url": "https://example.com/responses",
                        "title": "Responses source",
                    },
                },
                {
                    "type": "response.completed",
                    "response": {
                        "status": "completed",
                        "usage": {"input_tokens": 8, "output_tokens": 3},
                    },
                },
            ],
        )

    with Server([first, second]) as server:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_API_KEY": "response-key",
                "UAGENT_INTERNAL_WIRE_API": "responses",
                "UAGENT_INTERNAL_FEATURES": "hosted_search",
                "UAGENT_WEB_SEARCH_BACKEND": "auto",
                "UAGENT_PROVIDERS": json.dumps(
                    {
                        "search": {
                            "base_url": server.url,
                            "api_key": "fallback-key",
                            "supports": OPENROUTER_SUPPORTS,
                        }
                    }
                ),
                "UAGENT_WEB_SEARCH_MODEL": "search/fallback-model",
            }
        )
        result = run(root, env, "--yolo", "--json", "-p", "inspect and search", binary=binary)
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, (result.stderr, envelope))
        assert_true(envelope["answer"].startswith("responses-native-ok"), envelope)
        assert_true(envelope["usage"]["web_searches"] == 1, envelope)
        assert_true(len(server.requests) == 2, server.requests)
        sessions = {headers.get("X-Session-Id") for headers, _ in server.requests}
        assert_true(len(sessions) == 1 and None not in sessions, sessions)


def test_hosted_search_reports_one_lifecycle_on_either_route(root, home, *, binary):
    """A provider-run search must look the same whichever provider ran it.

    Without this the wait is indistinguishable from thinking: the decoders
    marked search events as progress and dropped everything that said what
    they were.
    """

    def responses_stream(handler, _body):
        write_sse_sequence(
            handler,
            [
                {
                    "type": "response.output_item.added",
                    "output_index": 0,
                    "item": {
                        "type": "web_search_call",
                        "id": "ws_1",
                        "status": "in_progress",
                    },
                },
                {
                    "type": "response.web_search_call.searching",
                    "output_index": 0,
                    "item_id": "ws_1",
                },
                {
                    "type": "response.web_search_call.completed",
                    "output_index": 0,
                    "item_id": "ws_1",
                },
                # Responses names the same finished search twice; a client
                # must see one completion, and usage must count one search.
                {
                    "type": "response.output_item.done",
                    "output_index": 0,
                    "item": {
                        "type": "web_search_call",
                        "id": "ws_1",
                        "status": "completed",
                    },
                },
                {"type": "response.output_text.delta", "delta": "responses-search-ok"},
                {
                    "type": "response.completed",
                    "response": {
                        "status": "completed",
                        "usage": {"input_tokens": 6, "output_tokens": 2},
                    },
                },
            ],
        )

    def anthropic_stream(handler, _body):
        write_sse_sequence(
            handler,
            [
                {"type": "message_start", "message": {"usage": {"input_tokens": 5}}},
                {
                    "type": "content_block_start",
                    "index": 0,
                    "content_block": {
                        "type": "server_tool_use",
                        "id": "srvtoolu_1",
                        "name": "web_search",
                        "input": {"query": "C++20"},
                    },
                },
                {"type": "content_block_stop", "index": 0},
                {
                    "type": "content_block_start",
                    "index": 1,
                    "content_block": {
                        "type": "web_search_tool_result",
                        "tool_use_id": "srvtoolu_1",
                        "content": [
                            {
                                "type": "web_search_result",
                                "url": "https://example.com/anthropic",
                            }
                        ],
                    },
                },
                {"type": "content_block_stop", "index": 1},
                {
                    "type": "content_block_start",
                    "index": 2,
                    "content_block": {"type": "text", "text": ""},
                },
                {
                    "type": "content_block_delta",
                    "index": 2,
                    "delta": {"type": "text_delta", "text": "anthropic-search-ok"},
                },
                {"type": "content_block_stop", "index": 2},
                {
                    "type": "message_delta",
                    "delta": {"stop_reason": "end_turn"},
                    "usage": {"output_tokens": 3},
                },
                {"type": "message_stop"},
            ],
        )

    def events_for(wire_api, handler):
        with Server([handler]) as server:
            env = base_env(home, server.url)
            env.update(
                {
                    "UAGENT_API_KEY": "search-key",
                    "UAGENT_INTERNAL_WIRE_API": wire_api,
                    "UAGENT_INTERNAL_FEATURES": "hosted_search",
                    "UAGENT_WEB_SEARCH_BACKEND": "auto",
                    "UAGENT_PROVIDERS": json.dumps(
                        {
                            "search": {
                                "base_url": server.url,
                                "api_key": "fallback-key",
                                "supports": OPENROUTER_SUPPORTS,
                            }
                        }
                    ),
                    "UAGENT_WEB_SEARCH_MODEL": "search/fallback-model",
                }
            )
            result = run(root, env, "--yolo", "--json-stream", "-p", "search", binary=binary)
            assert_true(result.returncode == 0, result.stderr)
            return [json.loads(line) for line in result.stdout.splitlines()]

    for wire_api, handler, search_id in (
        ("responses", responses_stream, "ws_1"),
        ("anthropic_messages", anthropic_stream, "srvtoolu_1"),
    ):
        records = events_for(wire_api, handler)
        searches = [r for r in records if r["type"] == "response.hosted_tool"]
        assert_true(len(searches) >= 2, (wire_api, [r["type"] for r in records]))
        assert_true(all(r["data"]["tool"] == "web_search" for r in searches), searches)
        assert_true(all(r["data"]["id"] == search_id for r in searches), searches)
        phases = [r["data"]["phase"] for r in searches]
        assert_true(phases[0] in {"started", "searching"}, (wire_api, phases))
        assert_true(phases[-1] == "completed", (wire_api, phases))
        # One completion, however many times the provider announced it.
        assert_true(phases.count("completed") == 1, (wire_api, phases))
        # The query is the model's own prose and never leaves the wire.
        assert_true(all("query" not in r["data"] for r in searches), searches)
        # A hosted search is not a round this agent owes an answer to.
        assert_true(
            not [r for r in records if r["type"] == "tool.call"],
            [r["type"] for r in records],
        )
        usage = next(r for r in records if r["type"] == "usage")
        assert_true(usage["data"]["usage"]["web_searches"] == 1, usage)


def test_provider_anthropic_native_search_pause_turn_replay(root, home, *, binary):
    def first(handler, body):
        assert_true(handler.path == "/v1/messages", handler.path)
        assert_true(handler.headers.get("x-api-key") == "anthropic-key", handler.headers)
        assert_true(handler.headers.get("anthropic-version") == "2023-06-01", handler.headers)
        assert_true(handler.headers.get("Authorization") is None, handler.headers)
        hosted = [tool for tool in body["tools"] if tool.get("name") == "web_search"]
        assert_true(
            hosted == [{"type": "web_search_20250305", "name": "web_search"}],
            hosted,
        )
        assert_true("system" in body and body["messages"][0]["role"] == "user", body)
        assert_true(body["system"][0]["cache_control"] == {"type": "ephemeral"}, body)
        assert_true(body["cache_control"] == {"type": "ephemeral"}, body)
        write_sse_sequence(
            handler,
            [
                {
                    "type": "message_start",
                    "message": {"usage": {"input_tokens": 5}},
                },
                {
                    "type": "content_block_start",
                    "index": 0,
                    "content_block": {
                        "type": "server_tool_use",
                        "id": "search-1",
                        "name": "web_search",
                        "input": {"query": "C++20"},
                    },
                },
                {"type": "content_block_stop", "index": 0},
                {
                    "type": "content_block_start",
                    "index": 1,
                    "content_block": {
                        "type": "web_search_tool_result",
                        "tool_use_id": "search-1",
                        "content": [
                            {
                                "type": "web_search_result",
                                "url": "https://example.com/anthropic",
                                "title": "Anthropic source",
                                "encrypted_content": "opaque-index",
                            }
                        ],
                    },
                },
                {"type": "content_block_stop", "index": 1},
                {
                    "type": "message_delta",
                    "delta": {"stop_reason": "pause_turn"},
                    "usage": {"output_tokens": 2},
                },
                {"type": "message_stop"},
            ],
        )

    def second(handler, body):
        assistant = body["messages"][-1]
        assert_true(assistant["role"] == "assistant", body["messages"])
        types = [block.get("type") for block in assistant["content"]]
        assert_true(types == ["server_tool_use", "web_search_tool_result"], assistant)
        result_block = assistant["content"][1]["content"][0]
        assert_true(result_block.get("encrypted_content") == "opaque-index", result_block)
        write_sse_sequence(
            handler,
            [
                {
                    "type": "message_start",
                    "message": {"usage": {"input_tokens": 9}},
                },
                {
                    "type": "content_block_start",
                    "index": 0,
                    "content_block": {"type": "text", "text": ""},
                },
                {
                    "type": "content_block_delta",
                    "index": 0,
                    "delta": {"type": "text_delta", "text": "anthropic-native-ok"},
                },
                {
                    "type": "content_block_delta",
                    "index": 0,
                    "delta": {
                        "type": "citations_delta",
                        "citation": {
                            "type": "web_search_result_location",
                            "url": "https://example.com/anthropic",
                            "title": "Anthropic source",
                            "cited_text": "source text",
                        },
                    },
                },
                {"type": "content_block_stop", "index": 0},
                {
                    "type": "message_delta",
                    "delta": {"stop_reason": "end_turn"},
                    "usage": {"output_tokens": 3},
                },
                {"type": "message_stop"},
            ],
        )

    with Server([first, second]) as server:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_API_KEY": "anthropic-key",
                "UAGENT_INTERNAL_WIRE_API": "anthropic_messages",
                "UAGENT_INTERNAL_FEATURES": "hosted_search",
                "UAGENT_WEB_SEARCH_BACKEND": "auto",
                "UAGENT_PROVIDERS": json.dumps(
                    {
                        "search": {
                            "base_url": server.url,
                            "api_key": "fallback-key",
                            "supports": OPENROUTER_SUPPORTS,
                        }
                    }
                ),
                "UAGENT_WEB_SEARCH_MODEL": "search/fallback-model",
            }
        )
        result = run(root, env, "--yolo", "--json", "-p", "search", binary=binary)
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, (result.stderr, envelope))
        assert_true(envelope["answer"].startswith("anthropic-native-ok"), envelope)
        assert_true(envelope["usage"]["web_searches"] == 1, envelope)
        assert_true(len(server.requests) == 2, server.requests)
        sessions = {headers.get("X-Session-Id") for headers, _ in server.requests}
        assert_true(len(sessions) == 1 and None not in sessions, sessions)


def test_uagent_tool_reports_live_configuration(root, home, *, binary):
    """uagent inspection answers from the running binary and never leaks secrets."""
    save_settings(home, UAGENT_MAX_TOOL_CALLS=120)

    def ask_config(_, body):
        assert_true("uagent" in function_names(body), function_names(body))
        return tool_call(
            "uagent", {"action": "inspect", "topic": "config", "name": "UAGENT_MAX_TOOL_CALLS"}
        )

    def ask_status(_, body):
        described = json.loads(tool_results(body["messages"])[-1])
        setting = described["settings"][0]
        assert_true(setting["name"] == "UAGENT_MAX_TOOL_CALLS", setting)
        assert_true(setting["effective"] == 120, setting)
        assert_true(setting["source"] == "user", setting)
        assert_true(setting["default"] == 0, setting)
        assert_true(setting["takes_effect"] == "next-user-turn", setting)
        return tool_call("uagent", {"action": "inspect", "topic": "status"}, call_id="call-2")

    def ask_prompt(_, body):
        status = json.loads(tool_results(body["messages"])[-1])
        assert_true(status["version"], status)
        assert_true(status["approval"] == "yolo", status)
        return tool_call("uagent", {"action": "inspect", "topic": "routes"}, call_id="call-3")

    def ask_routes(_, body):
        # A model that cannot read the route table guesses a selection, and a
        # guess resolving to the wrong endpoint fails as an auth error rather
        # than as the typo it is.
        described = json.loads(tool_results(body["messages"])[-1])
        named = {route["name"]: route for route in described["models"]}
        route = named.get("fixture-provider/fixture-route")
        assert_true(route is not None, described)
        assert_true(route["model"] == "fixture-model", described)
        assert_true(route["credential"] == "set", described)
        scoped = {provider["name"] for provider in described["providers"]}
        assert_true("fixture-provider" in scoped, described)
        assert_true(described["selection"] == "[provider/]model[:effort]", described)
        assert_true("high" in described["efforts"], described)
        # The table names routes; it never carries what authenticates them.
        assert_true("canary-route-key" not in json.dumps(described), described)
        return tool_call("uagent", {"action": "inspect", "topic": "prompt"}, call_id="call-4")

    def finish(_, body):
        described = json.loads(tool_results(body["messages"])[-1])
        prompt = body["messages"][0]["content"]
        assert_true(described["effective"] == prompt, described)
        assert_true(described["bytes"] == len(prompt.encode()), described)
        sources = {source["scope"]: source for source in described["sources"]}
        assert_true(sources["built-in"]["active"], sources)
        # Instructions ride in the repository context; no prompt documents.
        assert_true(set(sources) <= {"built-in", "repository"}, sources)
        serialized = json.dumps(body["messages"])
        assert_true("canary-api-key" not in serialized, "secret leaked into transcript")
        return event({"content": "self-info-ok"})

    with Server([ask_config, ask_status, ask_prompt, ask_routes, finish]) as server:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_API_KEY": "canary-api-key",
                "UAGENT_PROVIDERS": json.dumps(
                    {
                        "fixture-provider": {
                            "base_url": server.url,
                            "api_key": "canary-route-key",
                            "models": {"fixture-route": "fixture-model"},
                        }
                    }
                ),
            }
        )
        result = run(root, env, "--yolo", "-p", "describe yourself", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("self-info-ok"), result.stdout)
        assert_true("canary-route-key" not in result.stdout, result.stdout)


def test_effort_is_part_of_the_conversations_model_choice(root, home, *, binary):
    """/effort changes the model the conversation chose, at the same scope."""
    with Server([event({"content": "ready"})]) as server:
        env = base_env(home, server.url)
        session = run_dialog(
            root, env, "/effort high\n/config\n/effort default\n/config\n/quit\n", binary=binary
        )
        assert_true(session.returncode == 0, session.stderr)
        chosen = [line for line in session.stdout.splitlines() if "UAGENT_MODEL = " in line]
        assert_true(
            len(chosen) == 2
            and chosen[0].split(" · ")[0].endswith(":high")
            and not chosen[1].split(" · ")[0].endswith(":high")
            and all("conversation" in line for line in chosen),
            session.stdout,
        )
        # Nothing outside the conversation remembers it.
        assert_true(not saved_settings(home), "effort left the conversation")


def test_provider_summary_capability_fallback(root, home, *, binary):
    def reject(handler, body):
        assert_true(body["reasoning"]["summary"] == "auto", body)
        write_json_response(
            handler, {"error": {"message": "Unsupported parameter: reasoning.summary"}}, status=400
        )

    def answer(handler, body):
        assert_true("summary" not in body.get("reasoning", {}), body)
        write_sse_sequence(
            handler,
            [
                {"type": "response.output_text.delta", "delta": "summary-fallback-ok"},
                {"type": "response.completed", "response": {"status": "completed"}},
            ],
        )

    with Server([reject, answer]) as server:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_INTERNAL_WIRE_API": "responses",
                "UAGENT_INTERNAL_MODEL_FEATURES": '{"reasoning_summary":true}',
            }
        )
        result = run(root, env, "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("summary-fallback-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))


def catalog_of(context):
    return {"data": [{"id": "catalog-model", "context_length": context}]}


def window_of(result):
    return next(line for line in result.stdout.splitlines() if line.strip().startswith("context"))


def test_a_start_reuses_the_days_catalogue_instead_of_waiting_for_it(root, home, *, binary):
    delay = 4
    catalog = catalog_of(40000)

    def slow_catalog(_):
        time.sleep(delay)
        return catalog

    with Server([event({"content": "catalog-ok"})] * 3, get_response=slow_catalog) as server:
        env = {**base_env(home, server.url), "UAGENT_MODEL": "catalog-model"}
        del env["UAGENT_CONTEXT"]
        # The first start has no catalogue: it waits for the provider's.
        cold = run_dialog(root, env, "/status\n/q\n", timeout=3 * delay, binary=binary)
        assert_true(cold.returncode == 0, cold.stderr)
        assert_true("40k" in window_of(cold), cold.stdout)
        assert_true(len(server.get_requests) == 1, server.get_requests)

        # The next shows its prompt long before the provider would answer, and
        # runs on the model and window it learned.
        started = time.monotonic()
        prompt_at = []
        code, output = run_pty(
            root,
            env,
            [(b"probe\n", b"catalog-ok"), b"/q\n"],
            before_payload=lambda: prompt_at.append(time.monotonic() - started),
            binary=binary,
        )
        assert_true(code == 0, output[-2000:])
        assert_true(prompt_at[0] < delay - 1, prompt_at)
        assert_true(len(server.get_requests) == 1, server.get_requests)
        assert_true(server.requests[-1][1]["model"] == "catalog-model", server.requests[-1])
        warm = run_dialog(root, env, "/status\n/q\n", timeout=3 * delay, binary=binary)
        assert_true("40k" in window_of(warm), warm.stdout)

        # A fresh query of the catalogue, the picker's, keeps the copy current.
        catalog.update(catalog_of(80000))
        requests = len(server.get_requests)
        listed = run_dialog(root, env, "/models catalog\n/q\n", timeout=3 * delay, binary=binary)
        assert_true(listed.returncode == 0, listed.stderr)
        assert_true(len(server.get_requests) == requests + 1, server.get_requests)
        refreshed = run_dialog(root, env, "/status\n/q\n", timeout=3 * delay, binary=binary)
        assert_true("80k" in window_of(refreshed), refreshed.stdout)

        # A catalogue a day old is asked for again.
        requests = len(server.get_requests)
        for cached in (home / ".uagent" / "models").iterdir():
            old = time.time() - 25 * 3600
            os.utime(cached, (old, old))
        catalog.update(catalog_of(90000))
        stale = run_dialog(root, env, "/status\n/q\n", timeout=3 * delay, binary=binary)
        assert_true(len(server.get_requests) == requests + 1, server.get_requests)
        assert_true("90k" in window_of(stale), stale.stdout)

        # A model the kept catalogue does not list yet is looked up.
        requests = len(server.get_requests)
        catalog["data"].append({"id": "newer-model", "context_length": 16000})
        newer = run_dialog(
            root, {**env, "UAGENT_MODEL": "newer-model"}, "/status\n/q\n", binary=binary
        )
        assert_true(len(server.get_requests) == requests + 1, server.get_requests)
        assert_true("16k" in window_of(newer), newer.stdout)


def test_only_a_usable_catalogue_is_kept_and_read(root, home, *, binary):
    answers = [
        # An HTTP failure whose body looks like a catalogue.
        (500, catalog_of(11000)),
        (200, {"data": []}),
        (200, catalog_of(40000)),
        (200, catalog_of(50000)),
    ]

    def answer(handler):
        status, body = answers.pop(0)
        write_json_response(handler, body, status=status)

    with Server([event({"content": "catalog-ok"})], get_response=answer) as server:
        env = {**base_env(home, server.url), "UAGENT_MODEL": "catalog-model"}
        del env["UAGENT_CONTEXT"]
        models = home / ".uagent" / "models"

        for _ in range(2):
            run_dialog(root, env, "/q\n", binary=binary)
            assert_true(not list(models.glob("*")), list(models.glob("*")))

        started = run_dialog(root, env, "/status\n/q\n", binary=binary)
        assert_true("40k" in window_of(started), started.stdout)
        [cached] = models.glob("*")

        # A kept answer that holds no model is not read.
        kept = json.loads(cached.read_text())
        kept["response"] = {"data": [{}]}
        cached.write_text(json.dumps(kept))
        again = run_dialog(root, env, "/status\n/q\n", binary=binary)
        assert_true("50k" in window_of(again), again.stdout)
        assert_true(len(server.get_requests) == 4, server.get_requests)


def test_no_model_set_headless_lists_the_endpoints_models(root, home, *, binary):
    models = {"data": [{"id": f"model-{number:02d}"} for number in range(12)]}
    with Server([event({"content": "never asked"})], get_response=models) as server:
        env = base_env(home, server.url)
        del env["UAGENT_MODEL"]
        for flags in (("-p", "probe"), ("--json", "-p", "probe")):
            result = run(root, env, *flags, binary=binary)
            text = result.stdout + result.stderr
            assert_true(result.returncode != 0, text)
            assert_true("no model set: set `model` (UAGENT_MODEL)" in text, text)
            # Up to ten ids, in the endpoint's order, and a count of the rest.
            assert_true("model-00" in text and "model-09" in text, text)
            assert_true("model-10" not in text and "and 2 more" in text, text)
        assert_true(not server.requests, server.requests)


def test_a_conversation_without_a_model_starts_at_the_picker(root, home, *, binary):
    models = {"data": [{"id": "alpha"}, {"id": "beta"}]}
    with Server(
        [event({"content": "beta-answered"}), event({"content": "again"})], get_response=models
    ) as server:
        env = base_env(home, server.url)
        del env["UAGENT_MODEL"]
        # Said in words when a message is sent before choosing.
        skipped = run_dialog(root, env, "\x1b\nprobe\n/q\n", binary=binary)
        output = skipped.stdout + skipped.stderr
        assert_true("no model set: choose one with /models" in output, output)
        assert_true(not server.requests, server.requests)
        assert_true(not saved_settings(home), "nothing was chosen")

        result = run_dialog(root, env, "2\nprobe\n/q\n", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("beta-answered" in result.stdout, result.stdout)
        assert_true(server.requests[0][1]["model"] == "beta", server.requests)
        # The first choice is kept for the conversations after it.
        assert_true(saved_settings(home) == {"model": "beta"}, saved_settings(home))
        later = run_dialog(root, env, "probe\n/q\n", binary=binary)
        assert_true("again" in later.stdout, later.stdout + later.stderr)
        assert_true(server.requests[1][1]["model"] == "beta", server.requests)


def test_images_go_to_a_vision_model_of_the_same_endpoint(root, home, *, binary):
    picture = root / "shot.png"
    picture.write_bytes(SMALL_PNG)
    models = {
        "data": [
            {"id": "text-model", "input_modalities": ["text"]},
            {"id": "vision-model", "input_modalities": ["text", "image"]},
        ]
    }

    def describe(_, body):
        assert_true(body.get("model") == "vision-model", body.get("model"))
        return event({"content": "a terminal showing a stack trace"})

    def answer(_, body):
        assert_true(body.get("model") == "text-model", body.get("model"))
        assert_true("stack trace" in json.dumps(body["messages"]), body["messages"])
        return event({"content": "saw-it"})

    with Server([describe, answer], get_response=models) as server:
        env = base_env(home, server.url)
        env["UAGENT_MODEL"] = "text-model"
        del env["UAGENT_CONTEXT"]
        result = run(
            root, env, "--yolo", "--attach", str(picture), "-p", "what is this", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "saw-it", result.stdout)

    # No model of the endpoint accepts images: the person is told what to set.
    text_only = {"data": [{"id": "text-model", "input_modalities": ["text"]}]}
    with Server([event({"content": "blind"})], get_response=text_only) as server:
        env = base_env(home, server.url)
        env["UAGENT_MODEL"] = "text-model"
        del env["UAGENT_CONTEXT"]
        result = run(
            root,
            env,
            "--yolo",
            "--attach",
            str(picture),
            "--json-stream",
            "-p",
            "what",
            binary=binary,
        )
        assert_true(
            "no model on this endpoint accepts images; set image.model" in result.stdout,
            result.stdout + result.stderr,
        )
        assert_true(len(server.requests) == 1, server.requests)


def test_an_image_the_vision_model_cannot_read_is_tried_once_per_turn(root, home, *, binary):
    picture = root / "shot.png"
    picture.write_bytes(SMALL_PNG)
    models = {
        "data": [
            {"id": "text-model", "input_modalities": ["text"]},
            {"id": "vision-model", "input_modalities": ["text", "image"]},
        ]
    }
    answers = iter([tool_call("run", {"command": "true"}), event({"content": "done"})])

    def respond(handler, body):
        if body.get("model") == "vision-model":
            write_json_response(handler, {"error": {"message": "Could not process image"}}, 400)
            return None
        assert_true("[Image analysis failed" in json.dumps(body["messages"]), body["messages"])
        return next(answers)

    with Server([respond], get_response=models) as server:
        env = base_env(home, server.url)
        env["UAGENT_MODEL"] = "text-model"
        del env["UAGENT_CONTEXT"]
        result = run(root, env, "--yolo", "--attach", str(picture), "-p", "what", binary=binary)
        assert_true(result.stdout.strip() == "done", result.stdout + result.stderr)
        # Two steps of one turn, one attempt at the image: the failure is
        # remembered rather than paid again before every step.
        vision = [body for _, body in server.requests if body.get("model") == "vision-model"]
        assert_true(len(vision) == 1, [body.get("model") for _, body in server.requests])


def test_search_goes_to_an_endpoint_that_offers_it_and_otherwise_is_off(root, home, *, binary):
    def searched(_, body):
        assert_true(body["model"] == "search-model", body)
        return {"choices": [{"message": {"content": "found it"}, "finish_reason": "stop"}]}

    with Server([searched]) as search_server:
        providers = {
            "search": {
                "base_url": search_server.url,
                "api_key": "search-key",
                "supports": OPENROUTER_SUPPORTS,
            }
        }
        with Server(
            [tool_call("web_search", {"queries": ["current fact"]}), event({"content": "done"})]
        ) as plain:
            env = base_env(home, plain.url)
            env["UAGENT_PROVIDERS"] = json.dumps(providers)
            env["UAGENT_WEB_SEARCH_MODEL"] = "search-model"
            result = run(root, env, "--yolo", "-p", "search", binary=binary)
            assert_true(result.stdout.strip() == "done", result.stdout + result.stderr)
            assert_true("web_search" in function_names(plain.requests[0][1]), plain.requests)
            assert_true(len(search_server.requests) == 1, search_server.requests)

        # The endpoint's model names are not the conversation's to lend: with
        # none named for it, the call says what to set.
        def says_what_to_set(_, body):
            result = tool_results(body["messages"])[-1]
            assert_true("webSearch.model" in result, result)
            return event({"content": "told"})

        with Server(
            [tool_call("web_search", {"queries": ["current fact"]}), says_what_to_set]
        ) as plain:
            env = base_env(home, plain.url)
            env["UAGENT_PROVIDERS"] = json.dumps(providers)
            result = run(root, env, "--yolo", "-p", "search", binary=binary)
            assert_true(result.stdout.strip() == "told", result.stdout + result.stderr)
            assert_true(len(search_server.requests) == 1, search_server.requests)

    # No endpoint offers search: the tool is off.
    with Server([event({"content": "no tool"})]) as plain:
        result = run(root, base_env(home, plain.url), "--yolo", "-p", "search", binary=binary)
        assert_true(result.stdout.strip() == "no tool", result.stdout + result.stderr)
        assert_true("web_search" not in function_names(plain.requests[0][1]), plain.requests)


def test_search_is_offered_to_a_conversation_that_chose_its_model_late(root, home, *, binary):
    models = {"data": [{"id": "alpha"}]}
    with Server([event({"content": "answered"})], get_response=models) as server:
        env = base_env(home, server.url)
        del env["UAGENT_MODEL"]
        env["UAGENT_INTERNAL_FEATURES"] = ",".join(OPENROUTER_SUPPORTS)
        result = run_dialog(root, env, "1\nprobe\n/q\n", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("web_search" in function_names(server.requests[0][1]), server.requests)
