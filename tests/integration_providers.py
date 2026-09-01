from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    function_names,
    json,
    provider_env,
    run,
    run_dialog,
    run_pty,
    tool_call,
    tool_calls,
    tool_results,
    two_route_providers,
    write_json_response,
    write_sse_sequence,
)


def test_streamed_search_citations(root, home):
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
        result = run_dialog(root, base_env(home, server.url), "probe\nagain\n/trace\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("grounded\n  ← web_search" in result.stdout, result.stdout)
        assert_true("web_search ×1 · 1 source" in result.stdout, result.stdout)
        assert_true("legacy\n  ← 1 source" in result.stdout, result.stdout)
        assert_true("Sources:" in result.stdout, result.stdout)
        assert_true("https://example.com/source" in result.stdout, result.stdout)
        assert_true("legacy snippet" in result.stdout, result.stdout)


def test_openrouter_named_search_contract_and_errors(root, home):
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
        assert_true("web_search OpenRouter HTTP 429: rate limited" in result, result)
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
                    "UAGENT_OPENROUTER_COMPATIBLE": "1",
                    "UAGENT_WEB_SEARCH_BACKEND": "openrouter",
                    "UAGENT_WEB_SEARCH_URL": search_server.url,
                    "UAGENT_WEB_SEARCH_API_KEY": "search-key",
                    "UAGENT_WEB_SEARCH_MODEL": "search-model",
                }
            )
            result = run(root, env, "--yolo", "--json", "-p", "search")
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


def test_openrouter_reasoning_details_survive_tool_step(root, home):
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
        env["UAGENT_OPENROUTER_COMPATIBLE"] = "1"
        result = run(root, env, "-p", "inspect")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "openrouter-replay-ok", result.stdout)


def test_provider_context_overflow_compacts_once(root, home):
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
        result = run(root, base_env(home, server.url), "-p", prompt, timeout=15)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "context-recovery-ok", result.stdout)
        assert_true(len(server.requests) == 3, server.requests)

    def reject_413(handler, _):
        write_json_response(handler, {"error": "request too large"}, status=413)

    # If the bounded compaction request is also rejected, do not loop or replay
    # the original request. A later user turn can retry deliberately.
    with Server([reject_413]) as server:
        failed = run(root, base_env(home, server.url), "-p", prompt, timeout=15)
        assert_true(failed.returncode == 1, (failed.stdout, failed.stderr))
        assert_true(len(server.requests) == 2, server.requests)


def test_provider_background_completion_does_not_trigger_model_turns(root, home):
    def launch(_, __):
        return tool_calls(
            [
                (
                    "first-bg",
                    "run",
                    {"command": "sleep 0.6; printf first", "yield_ms": 250},
                ),
                (
                    "second-bg",
                    "run",
                    {"command": "sleep 1.6; printf second", "yield_ms": 250},
                ),
            ]
        )

    with Server([launch, event({"content": "background-launched"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MEMORY"] = "0"
        code, output = run_pty(
            root,
            env,
            [
                (b"go\n", b"background-launched"),
                (b"", b"second"),
                b"/q\n",
            ],
            timeout=10,
            args=("--yolo",),
        )
        assert_true(code == 0, output)
        assert_true(b"first" in output and b"second" in output, output)
        assert_true(len(server.requests) == 2, server.requests)


def test_provider_text_protocol_preserves_reasoning_and_trace(root, home):
    trace_path = root / "text-protocol-debug.jsonl"
    call_markup = (
        "[uagent_tool_call]"
        + json.dumps({"name": "read_path", "arguments": {"path": "."}})
        + "[/uagent_tool_call]"
    )

    def reject_native_tools(handler, body):
        assert_true("tools" in body, body)
        write_json_response(
            handler,
            {"error": {"message": "tools are unsupported", "type": "invalid_request_error"}},
            status=400,
        )

    def text_call_response(_, body):
        assert_true("tools" not in body, body)
        return event(
            {"reasoning_content": "direct provider reasoning", "content": call_markup},
            finish="stop",
        )

    def finish(_, body):
        assistant = next(
            message
            for message in reversed(body["messages"])
            if message.get("role") == "assistant" and message.get("content") == call_markup
        )
        assert_true(assistant.get("reasoning_content") == "direct provider reasoning", assistant)
        results = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
            and str(message.get("content", "")).startswith("[tool_result read_path]")
        ]
        return event({"content": "text-provider-ok" if results else "text-provider-bad"})

    with Server([reject_native_tools, text_call_response, finish]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "--json",
            f"--debug={trace_path}",
            "-p",
            "inspect",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("debug trace:" in result.stderr, result.stderr)
        # The binary prints the absolute path it opened, not a canonical one,
        # so on macOS a symlinked TMPDIR makes `/var` and `/private/var`
        # both correct spellings of the same file.
        assert_true(
            str(trace_path) in result.stderr or str(trace_path.resolve()) in result.stderr,
            result.stderr,
        )
        envelope = json.loads(result.stdout)
        assert_true(envelope["answer"] == "text-provider-ok", envelope)
        assert_true(len(envelope["trace"]) == 1, envelope)
        call = envelope["trace"][0]
        assert_true(call["name"] == "read_path", call)
        assert_true(call["arguments"] == {"path": "."}, call)
        assert_true(call["text_protocol"], call)
        assert_true(" entries " in call["result"], call)
        assert_true("uagent_tool_call" not in json.dumps(envelope["trace"]), envelope)

        records = [json.loads(line) for line in trace_path.read_text().splitlines()]
        ready = next(record["data"] for record in records if record["event"] == "session_ready")
        assert_true(ready["run_mode"] == "headless" and ready["output_mode"] == "json", ready)
        requests = [record["data"] for record in records if record["event"] == "model_request"]
        assert_true(any("schema_snapshot" in request for request in requests), requests)
        tool_events = [
            record["data"] for record in records if record["event"] in {"tool_call", "tool_result"}
        ]
        assert_true(len(tool_events) == 2, tool_events)
        assert_true(tool_events[0]["presentation"]["title"] == "read_path", tool_events)
        assert_true(tool_events[1]["presentation"]["status"] == "succeeded", tool_events)
        responses = [record["data"] for record in records if record["event"] == "model_response"]
        assert_true(
            any(
                response.get("reasoning_content_field")
                and response.get("reasoning") == "direct provider reasoning"
                for response in responses
            ),
            responses,
        )


def test_model_route_switch(root, home):
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
            root,
            env,
            "/models all\n2\n/effort default\n/effort high\nprobe\n/q\n",
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


def test_openrouter_variant_is_scoped_to_openrouter(root, home):
    router = Server(
        [
            event({"content": "nitro-ok"}),
            event({"content": "floor-ok"}),
            event({"content": "exacto-ok"}),
            event({"content": "default-ok"}),
        ]
    )
    generic = Server([event({"content": "generic-ok"})])
    providers = {
        "router": {
            "base_url": router.url,
            "api_key": "router-key",
            "protocol": "openrouter",
            "models": {"main": "vendor/model"},
        },
        "generic": {
            "base_url": generic.url,
            "api_key": "generic-key",
            "models": {"main": "other/model"},
        },
    }
    try:
        env = provider_env(home, router.url, providers, "router/main")
        result = run_dialog(
            root,
            env,
            "/variant\n/variant :nitro\none\n/variant floor\ntwo\n"
            "/variant exacto\nthree\n/variant default\nfour\n"
            "/model generic/main\n/variant nitro\nfive\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("choose default, nitro, floor, or exacto" in result.stdout, result.stdout)
        assert_true("/variant is unavailable on the active route" in result.stdout, result.stdout)
        for reply in ("nitro-ok", "floor-ok", "exacto-ok", "default-ok", "generic-ok"):
            assert_true(reply in result.stdout, result.stdout)
        router_bodies = [body for _, body in router.requests]
        assert_true(
            [body.get("model") for body in router_bodies]
            == [
                "vendor/model:nitro",
                "vendor/model:floor",
                "vendor/model:exacto",
                "vendor/model",
            ],
            router_bodies,
        )
        assert_true(generic.requests[0][1].get("model") == "other/model", generic.requests)
        session_ids = [body.get("session_id") for body in router_bodies]
        assert_true(all(session_ids) and len(set(session_ids)) == 4, session_ids)
    finally:
        router.close()
        generic.close()


def test_dynamic_provider_catalog_and_model(root, home):
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
        catalog_result = run_dialog(
            root,
            env,
            "/models live\n\x1b\nprobe\n/q\n",
        )
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

        wildcard = run_dialog(
            root,
            env,
            "/models second/*\n\x1b\n/q\n",
        )
        assert_true(wildcard.returncode == 0, wildcard.stderr)
        assert_true("second/gpt-live" in wildcard.stdout, wildcard.stdout)

        selected = run_dialog(
            root,
            env,
            "/models gpt-live\n1\nprobe\n/q\n",
        )
        assert_true(selected.returncode == 0, selected.stderr)
        assert_true("dynamic-route-ok" in selected.stdout, selected.stdout)
        assert_true(len(second.get_requests) == 3, second.get_requests)

        restart_env = provider_env(home, first.url, providers)
        restarted = run(root, restart_env, "-p", "probe")
        assert_true(restarted.returncode == 0, restarted.stderr)
        assert_true(restarted.stdout.strip() == "dynamic-route-ok", restarted.stdout)
        assert_true(len(first.requests) == 1, first.requests)
    finally:
        first.close()
        second.close()


def test_model_preference_survives_restart(root, home):
    first = Server([event({"content": "explicit-model-ok"})])

    def remembered(_, body):
        valid = body.get("model") == "model-b" and body.get("reasoning_effort") == "medium"
        return event({"content": "remembered-model-ok" if valid else "remembered-model-bad"})

    second = Server([remembered])
    providers = two_route_providers(first.url, second.url)
    providers["second"]["context"] = 8192
    try:
        choose_env = provider_env(home, first.url, providers, "first/main")
        chosen = run_dialog(root, choose_env, "/model second/fast\n/q\n")
        assert_true(chosen.returncode == 0, chosen.stderr)

        preference = home / ".uagent" / "config" / "model-preference.json"
        saved = json.loads(preference.read_text(encoding="utf-8"))
        assert_true(saved["selection"] == "second/fast" and saved["route"], saved)
        assert_true(preference.stat().st_mode & 0o777 == 0o600, oct(preference.stat().st_mode))

        restart_env = provider_env(home, first.url, providers)
        restarted = run(root, restart_env, "-p", "probe")
        assert_true(restarted.returncode == 0, restarted.stderr)
        assert_true(restarted.stdout.strip() == "remembered-model-ok", restarted.stdout)
        assert_true(not first.requests, first.requests)

        override_env = dict(restart_env)
        override_env["UAGENT_MODEL"] = "first/main"
        overridden = run(root, override_env, "-p", "probe")
        assert_true(overridden.returncode == 0, overridden.stderr)
        assert_true(overridden.stdout.strip() == "explicit-model-ok", overridden.stdout)
    finally:
        first.close()
        second.close()


def test_provider_responses_native_search_and_function_replay(root, home):
    def first(handler, body):
        assert_true(handler.path == "/v1/responses", handler.path)
        assert_true(handler.headers.get("Authorization") == "Bearer response-key", handler.headers)
        assert_true("input" in body and "messages" not in body, body)
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
                "UAGENT_WIRE_API": "responses",
                "UAGENT_HOSTED_TOOLS": "web_search",
                "UAGENT_WEB_SEARCH_BACKEND": "auto",
                "UAGENT_WEB_SEARCH_URL": server.url,
                "UAGENT_WEB_SEARCH_API_KEY": "fallback-key",
            }
        )
        result = run(root, env, "--yolo", "--json", "-p", "inspect and search")
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, (result.stderr, envelope))
        assert_true(envelope["answer"].startswith("responses-native-ok"), envelope)
        assert_true(envelope["usage"]["web_searches"] == 1, envelope)
        assert_true(len(server.requests) == 2, server.requests)


def test_provider_anthropic_native_search_pause_turn_replay(root, home):
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
                "UAGENT_WIRE_API": "anthropic_messages",
                "UAGENT_HOSTED_TOOLS": "web_search",
                "UAGENT_WEB_SEARCH_BACKEND": "auto",
                "UAGENT_WEB_SEARCH_URL": server.url,
                "UAGENT_WEB_SEARCH_API_KEY": "fallback-key",
            }
        )
        result = run(root, env, "--yolo", "--json", "-p", "search")
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, (result.stderr, envelope))
        assert_true(envelope["answer"].startswith("anthropic-native-ok"), envelope)
        assert_true(envelope["usage"]["web_searches"] == 1, envelope)
        assert_true(len(server.requests) == 2, server.requests)


def test_self_info_reports_live_configuration(root, home):
    """uagent_info answers from the running binary and never leaks secrets."""
    config = home / ".uagent"
    config.mkdir(parents=True, exist_ok=True)
    (config / ".config").write_text("UAGENT_MAX_TOOL_CALLS=120\n")

    def ask_config(_, body):
        assert_true("uagent_info" in function_names(body), function_names(body))
        return tool_call("uagent_info", {"topic": "config", "name": "UAGENT_MAX_TOOL_CALLS"})

    def ask_status(_, body):
        described = json.loads(tool_results(body["messages"])[-1])
        setting = described["settings"][0]
        assert_true(setting["name"] == "UAGENT_MAX_TOOL_CALLS", setting)
        assert_true(setting["active"] == 120, setting)
        assert_true(setting["source"] == "global-config", setting)
        assert_true(setting["default"] == 0, setting)
        assert_true(setting["takes_effect"] == "next-user-turn", setting)
        return tool_call("uagent_info", {"topic": "status"}, call_id="call-2")

    def ask_prompt(_, body):
        status = json.loads(tool_results(body["messages"])[-1])
        assert_true(status["version"], status)
        assert_true(status["approval"] == "yolo", status)
        return tool_call("uagent_info", {"topic": "routes"}, call_id="call-3")

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
        assert_true(described["selection"] == "[provider/]model[:variant][:effort]", described)
        assert_true("high" in described["efforts"], described)
        # The table names routes; it never carries what authenticates them.
        assert_true("canary-route-key" not in json.dumps(described), described)
        return tool_call("uagent_info", {"topic": "prompt"}, call_id="call-4")

    def finish(_, body):
        described = json.loads(tool_results(body["messages"])[-1])
        prompt = body["messages"][0]["content"]
        assert_true(described["base"]["chars"] > 1000, described)
        assert_true(len(described["base"]["digest"]) == 12, described)
        assert_true("## Evidence" in described["base"]["sections"], described)
        assert_true(described["overlay"]["applied"] == [], described)
        assert_true(described["overlay"]["path"] == "", described)
        # The reported sizes must match the message the model actually got.
        # Triggers are registry names: `activity` is registered but its schema
        # is advertised only once a detached activity exists, so this is
        # deliberately not compared against the advertised schema list.
        start = prompt.find("\n\n## Capabilities\n")
        host = prompt.find("\n\n[HOST CAPABILITIES]")
        assert_true(start >= 0 and host > start, prompt[:200])
        assert_true(described["capabilities"]["chars"] == host - start, described)
        assert_true(
            described["host_capabilities"]["chars"] == len(prompt) - host,
            described,
        )
        serialized = json.dumps(body["messages"])
        assert_true("canary-search-key" not in serialized, "secret leaked into transcript")
        assert_true("canary-api-key" not in serialized, "secret leaked into transcript")
        return event({"content": "self-info-ok"})

    with Server([ask_config, ask_status, ask_prompt, ask_routes, finish]) as server:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_API_KEY": "canary-api-key",
                "UAGENT_WEB_SEARCH_API_KEY": "canary-search-key",
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
        result = run(root, env, "--yolo", "-p", "describe yourself")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("self-info-ok"), result.stdout)
        assert_true("canary-search-key" not in result.stdout, result.stdout)
        assert_true("canary-route-key" not in result.stdout, result.stdout)


def test_effort_and_variant_persist_like_model(root, home):
    """/effort updates the saved selection instead of evaporating on restart."""
    preference = home / ".uagent" / "config" / "model-preference.json"
    preference.parent.mkdir(parents=True, exist_ok=True)
    preference.write_text(
        json.dumps({"format": 1, "selection": "demo-model", "base_url": "", "route": False})
    )

    with Server([event({"content": "ready"})]) as server:
        env = base_env(home, server.url)
        session = run_dialog(root, env, "/effort high\n/quit\n")
        assert_true(session.returncode == 0, session.stderr)
        saved = json.loads(preference.read_text())
        assert_true(saved["selection"] == "demo-model:high", saved)

        # Clearing back to the provider default rewrites the same entry.
        session = run_dialog(root, env, "/effort default\n/quit\n")
        assert_true(session.returncode == 0, session.stderr)
        saved = json.loads(preference.read_text())
        assert_true(saved["selection"] == "demo-model", saved)

    # With nothing saved, the command says so rather than implying persistence.
    preference.unlink()
    with Server([event({"content": "ready"})]) as server:
        env = base_env(home, server.url)
        session = run_dialog(root, env, "/effort high\n/quit\n")
        assert_true("this session only" in session.stdout, session.stdout)
        assert_true(not preference.exists(), "must not invent a preference")
