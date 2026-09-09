// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <limits>
#include <string>
#include <vector>

#include "include/agent/trace.h"
#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/api/stream.h"
#include "include/tools/web_fetch.h"
#include "include/tools/web_search.h"
#include "include/ui/display.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestOpenRouterServerSearch() {
  RuntimeConfig config;
  Api api(config);
  api.base_url = "https://openrouter.ai/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  api.model = "vendor/model";
  json schemas = json::array(
      {{{"type", "function"},
        {"function", {{"name", "web_search"}, {"parameters", json::object()}}}},
       {{"type", "function"},
        {"function",
         {{"name", "read_file"}, {"parameters", json::object()}}}}});

  bool web_available = false;
  json body = api.BuildRequestBody(json::array(), schemas, "", &web_available);
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][0]["function"]["name"] == "web_search");
  CHECK(web_available);

  web_available = true;
  body = api.BuildRequestBody(json::array(), json::array({schemas[1]}), "",
                              &web_available);
  CHECK(body["tools"].size() == 1);
  CHECK(body["tools"][0]["function"]["name"] == "read_file");
  CHECK(!web_available);

  auto check_native_search = [&] {
    body = api.BuildRequestBody(json::array(), schemas);
    CHECK(body["tools"].size() == 2);
    CHECK(body["tools"][0]["function"]["name"] == "web_search");
  };
  api.base_url = "http://127.0.0.1:8080/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, api.base_url);
  check_native_search();

  api.base_url = "http://127.0.0.1:8787/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  check_native_search();

  api.base_url = "https://openrouter.ai/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  body = api.BuildRequestBody(json::array(), json::array());
  CHECK(!body.contains("tools"));  // compact/title requests stay tool-free

  Usage usage;
  usage.Add({{"prompt_tokens", 1},
             {"completion_tokens", 2},
             {"server_tool_use_details", {{"web_search_requests", 3}}}});
  CHECK(usage.web_searches == 3);
  usage.Add({{"server_tool_use", {{"web_search_requests", 2}}}});
  CHECK(usage.web_searches == 5);
  usage.Add({{"server_tool_use_details", {{"web_search_requests", 1}}},
             {"server_tool_use", {{"web_search_requests", 9}}}});
  CHECK(usage.web_searches == 6);
  CHECK(!usage.cost_reported);
  CHECK(UsageFromJson(UsageJson(usage)).web_searches == 6);
  usage.Add({{"cost", 0.0}});
  CHECK(usage.cost_reported);
  CHECK(UsageFromJson(UsageJson(usage)).cost_reported);

  // Provider and persisted usage are untrusted accounting inputs. Negative
  // values cannot reduce a budget, and huge totals saturate instead of
  // wrapping into negative numbers.
  Usage hostile;
  hostile.Add({{"prompt_tokens", -10},
               {"completion_tokens", -20},
               {"cache_read_input_tokens", -30},
               {"cost", -4.0},
               {"server_tool_use_details", {{"web_search_requests", -5}}}});
  CHECK(hostile.input == 0);
  CHECK(hostile.output == 0);
  CHECK(hostile.cache_read == 0);
  CHECK(hostile.web_searches == 0);
  CHECK(hostile.cost == 0);
  CHECK(!hostile.cost_reported);
  Usage enormous;
  enormous.output = std::numeric_limits<int64_t>::max();
  enormous.reasoning = std::numeric_limits<int64_t>::max();
  enormous.input = std::numeric_limits<int64_t>::max();
  enormous.cache_read = std::numeric_limits<int64_t>::max();
  CHECK(enormous.GeneratedTokens() == std::numeric_limits<int64_t>::max());
  CHECK(enormous.CacheHitPercent() == 50);
  enormous.Merge(enormous);
  CHECK(enormous.output == std::numeric_limits<int64_t>::max());
  Usage restored = UsageFromJson(
      {{"input", -1}, {"output", -2}, {"cost", -3.0}, {"web_searches", -4}});
  CHECK(restored.input == 0 && restored.output == 0);
  CHECK(restored.cost == 0 && restored.web_searches == 0);

  // Every OpenAI-compatible spelling must land on the same invariant: `input`
  // excludes cache reads and writes, so their sum is the whole prompt, and
  // the hit percentage is the cached share of it. Chat Completions and
  // Responses report cached tokens inside the prompt total; Anthropic-style
  // reports them beside an input count that already excludes them, and
  // subtracting there would under-report fresh tokens.
  struct UsageCase {
    const char* spelling;
    json payload;
    int64_t input;
    int64_t output;
    int64_t cache_read;
    int64_t cache_write;
    int64_t reasoning;
    int64_t cache_hit_percent;
  };
  const std::vector<UsageCase> usage_cases = {
      // The percentage is defined as zero before anything is counted.
      {"nothing counted", json::object(), 0, 0, 0, 0, 0, 0},
      // Compatibility providers occasionally report detail counts larger than
      // the parent totals; the result is clamped, never negative.
      {"chat completions, inconsistent details",
       json{{"prompt_tokens", 2},
            {"prompt_tokens_details", {{"cached_tokens", 3}}},
            {"completion_tokens", 1},
            {"completion_tokens_details", {{"reasoning_tokens", 2}}}},
       0, 0, 3, 0, 2, 100},
      {"responses",
       json{{"input_tokens", 11},
            {"input_tokens_details", {{"cached_tokens", 3}}},
            {"output_tokens", 7},
            {"output_tokens_details", {{"reasoning_tokens", 2}}}},
       8, 5, 3, 0, 2, 27},
      {"anthropic",
       json{{"input_tokens", 8},
            {"output_tokens", 5},
            {"cache_read_input_tokens", 3},
            {"cache_creation_input_tokens", 7}},
       8, 5, 3, 7, 0, 16},
      {"anthropic translated to chat completions",
       json{{"prompt_tokens", 18},
            {"prompt_tokens_details",
             {{"cached_tokens", 3}, {"cache_creation_tokens", 7}}},
            {"completion_tokens", 5}},
       8, 5, 3, 7, 0, 16},
      {"fully cached prompt",
       json{{"prompt_tokens", 10},
            {"prompt_tokens_details", {{"cached_tokens", 10}}}},
       0, 0, 10, 0, 0, 100},
  };
  for (const UsageCase& usage_case : usage_cases) {
    Usage accounted;
    accounted.Add(usage_case.payload);
    CHECK(accounted.input == usage_case.input);
    CHECK(accounted.output == usage_case.output);
    CHECK(accounted.cache_read == usage_case.cache_read);
    CHECK(accounted.cache_write == usage_case.cache_write);
    CHECK(accounted.reasoning == usage_case.reasoning);
    CHECK(accounted.CacheHitPercent() == usage_case.cache_hit_percent);
    // Reasoning is billed output even though `output` excludes it.
    CHECK(accounted.GeneratedTokens() ==
          usage_case.output + usage_case.reasoning);
  }

  // The status row is one ordered list: everything fits when there is room,
  // and the least valuable segments go first when there is not.
  RuntimeConfig status_config;
  Api status_api(status_config);
  status_api.base_url = "https://openrouter.ai/api/v1";
  status_api.model = "vendor/model";
  status_api.ctx_window = 1000000;
  Usage status_usage;
  status_usage.input = 1200000;
  status_usage.output = 45300;
  status_usage.cache_read = 3100000;
  status_usage.cost = 0.31;
  StatusView status_view{.context_used = 4700,
                         .model = "openrouter/vendor/model:high",
                         .host = {},
                         .yolo = true};
  setenv("COLUMNS", "200", 1);
  std::string wide = StatusBar(status_api, status_usage, status_view);
  CHECK(wide.find("ctx 4.7k/1M") != std::string::npos);
  CHECK(wide.find("1.2M in · 45.3k out") != std::string::npos);
  CHECK(wide.find("cache 72%") != std::string::npos);
  CHECK(wide.find("openrouter/vendor/model:high") != std::string::npos);
  CHECK(wide.find("YOLO") != std::string::npos);
  // An unknown context window degrades to the used figure alone.
  status_api.ctx_window = 0;
  CHECK(StatusBar(status_api, status_usage, status_view).find("ctx 4.7k ") !=
        std::string::npos);
  unsetenv("COLUMNS");

  // A configured search endpoint outranks the conversation's own route.
  RuntimeConfig search_config;
  search_config.web_search_url = "https://search.example/v1/";
  search_config.web_search_api_key = "search-key";
  search_config.web_search_model = "search-model";
  Api search_api(search_config);
  search_api.base_url = "https://inference.example/v1";
  search_api.api_key = "inference-key";
  WebSearchRoute route = SelectWebSearchRoute(search_api, {});
  CHECK(route.base_url == "https://search.example/v1");
  CHECK(route.api_key == "search-key");
  CHECK(route.model == "search-model");
  // A provider-scoped selection is a route of its own: endpoint, key and model
  // all come from it, and the :effort suffix beats the session default.
  setenv("UAGENT_PROVIDERS",
         R"json({"seeker":{"base_url":"https://seek.example/v1",
                            "api_key":"seek-key",
                            "protocol":"openrouter"},
                 "plain":{"base_url":"https://plain.example/v1",
                           "api_key":"plain-key"}})json",
         1);
  RuntimeConfig scoped_config = search_config;
  scoped_config.web_search_url.clear();
  scoped_config.web_search_api_key.clear();
  scoped_config.web_search_model = "seeker/finder-model:high";
  scoped_config.web_search_effort = "low";
  Api scoped_api(scoped_config);
  scoped_api.base_url = "https://inference.example/v1";
  scoped_api.api_key = "inference-key";
  WebSearchRoute scoped = SelectWebSearchRoute(scoped_api, {});
  CHECK(scoped.base_url == "https://seek.example/v1");
  CHECK(scoped.api_key == "seek-key");
  CHECK(scoped.model == "finder-model");
  CHECK(scoped.effort == "high");
  CHECK(WebSearchRequest(scoped, scoped_config, "q")["reasoning"]["effort"] ==
        "high");
  // A selection scoped to a provider that does not speak the OpenRouter
  // protocol disables search rather than searching somewhere else.
  RuntimeConfig foreign_config = scoped_config;
  foreign_config.web_search_model = "plain/finder-model";
  Api foreign_api(foreign_config);
  foreign_api.base_url = "https://inference.example/v1";
  foreign_api.api_key = "inference-key";
  CHECK(!SelectWebSearchRoute(foreign_api, {}).Valid());
  // A bare id still only renames the model on the winning candidate.
  RuntimeConfig bare_config = search_config;
  bare_config.web_search_model = "plain-model";
  Api bare_api(bare_config);
  bare_api.base_url = "https://inference.example/v1";
  bare_api.api_key = "inference-key";
  WebSearchRoute bare = SelectWebSearchRoute(bare_api, {});
  CHECK(bare.base_url == "https://search.example/v1");
  CHECK(bare.model == "plain-model");
  unsetenv("UAGENT_PROVIDERS");
  // Without any configured search route, an OpenRouter conversation route is
  // the search route; an OpenAI-protocol one leaves search unavailable.
  RuntimeConfig inherit_config;
  Api inherit_api(inherit_config);
  inherit_api.base_url = "https://openrouter.ai/api/v1";
  inherit_api.api_key = "inference-key";
  inherit_api.model = "vendor/model";
  inherit_api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, inherit_api.base_url);
  CHECK(SelectWebSearchRoute(inherit_api, {}).model == "vendor/model");
  inherit_api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, inherit_api.base_url);
  CHECK(!SelectWebSearchRoute(inherit_api, {}).Valid());
  // `off` refuses every candidate.
  RuntimeConfig disabled_config = search_config;
  disabled_config.web_search_backend = "off";
  Api disabled_api(disabled_config);
  CHECK(!SelectWebSearchRoute(disabled_api, {}).Valid());

  RuntimeConfig openrouter_config;
  openrouter_config.web_search_engine = "exa";
  openrouter_config.web_search_context_size = "high";
  WebSearchRoute openrouter_route{"https://openrouter.ai/api/v1", "key",
                                  "vendor/search-model", ""};
  json openrouter_body =
      WebSearchRequest(openrouter_route, openrouter_config, "current facts");
  CHECK(openrouter_body["model"] == "vendor/search-model");
  CHECK(openrouter_body["tools"].size() == 1);
  CHECK(openrouter_body["tools"][0]["type"] == "openrouter:web_search");
  CHECK(openrouter_body["tools"][0]["parameters"]["engine"] == "exa");
  CHECK(openrouter_body["tools"][0]["parameters"]["max_uses"] == 3);
  CHECK(openrouter_body["tools"][0]["parameters"]["max_results"] == 5);
  CHECK(openrouter_body["tools"][0]["parameters"]["max_total_results"] == 15);
  CHECK(openrouter_body["tools"][0]["parameters"]["search_context_size"] ==
        "high");
  CHECK(openrouter_body["max_tool_calls"] == 3);
  UsageAccumulator side_usage;
  Tool search_tool = WebSearchTool(api, side_usage, {});
  // The configured budget bounds one attempt; the tool deadline covers all of
  // them, or a retry would be cancelled before it ran.
  CHECK(search_tool.timeout_s == config.web_search_timeout_s * kSideAttempts);
  CHECK(search_tool.parameters["properties"]["queries"]["maxItems"] == 3);
  CHECK(search_tool.parameters["required"] == json::array({"queries"}));
  CHECK(!search_tool.parameters["properties"].contains("query"));
  auto missing_queries = FindToolArgumentIssue(search_tool, json::object());
  CHECK(missing_queries && missing_queries->code == "schema.required");
  CHECK(missing_queries && missing_queries->field == "queries");
  auto legacy_query = FindToolArgumentIssue(
      search_tool, {{"query", "legacy"}, {"queries", {"current"}}});
  CHECK(legacy_query && legacy_query->code == "schema.additional_property");
  CHECK(search_tool.summary({{"queries", {"one", "two"}}}) == "one | two");
  CHECK(!search_tool.mutating);
  CHECK(search_tool.needs_approval &&
        search_tool.needs_approval(json::object()));

  WebSearchResult normalized = ParseWebSearch(
      {{"choices",
        json::array({{{"finish_reason", "length"},
                      {"message",
                       {{"content", "grounded answer"},
                        {"annotations",
                         json::array({{{"type", "url_citation"},
                                       {"url_citation",
                                        {{"url", "https://example.com/source"},
                                         {"title", "Source"}}}}})}}}}})}});
  CHECK(normalized.text == "grounded answer");
  CHECK(normalized.searches == 1);
  CHECK(normalized.truncated);
  CHECK(CitationEntries(normalized.annotations).size() == 1);
  // A body without choices is a failed search, not an empty answer.
  CHECK(ParseWebSearch({{"error", {{"message", "nope"}}}}).searches == 0);

  Tool fetch_tool = WebFetchTool(api);
  CHECK(!fetch_tool.mutating);
  CHECK(fetch_tool.needs_approval && fetch_tool.needs_approval(json::object()));
  ToolContext fetch_context;
  CHECK(fetch_tool.run({{"url", "file:///etc/passwd"}}, fetch_context).error ==
        ToolErrorCode::kInvalidArguments);
  CHECK(fetch_tool.run({{"url", "example.com"}}, fetch_context).error ==
        ToolErrorCode::kInvalidArguments);

  // Address policy is enforced on libcurl's resolved connection target, not
  // only on the URL spelling. Cover private, link-local, documentation, IPv6,
  // IPv4-mapped and NAT64 forms without depending on a listening service.
  for (std::string_view url : {
           "http://127.0.0.1/",
           "http://10.0.0.1/",
           "http://169.254.169.254/latest/meta-data/",
           "http://192.0.2.1/",
           "http://[::1]/",
           "http://[fd00::1]/",
           "http://[::ffff:127.0.0.1]/",
           "http://[64:ff9b::7f00:1]/",
       }) {
    WebResponse denied = api.GetUrl(std::string(url), 1, 1024);
    CHECK(denied.error == "refused non-public network destination");
  }

  // Markup out, reading order in: dropped elements take their content with
  // them, block edges become line breaks, and inline tags do not split words.
  CHECK(HtmlToText("<p>one</p><p>two</p>") == "one\n\ntwo");
  CHECK(HtmlToText("<style>p{color:red}</style><p>kept</p>") == "kept");
  CHECK(HtmlToText("<script>if (a < b) doc('</p>')</script>text") == "text");
  CHECK(HtmlToText("<!-- note --><b>bo</b>ld") == "bold");
  CHECK(HtmlToText("a &amp; b &lt;c&gt; &#39;d&#39;") == "a & b <c> 'd'");
  // Source layout adds nothing: a block break is one blank line at most,
  // however many newlines and tags produced it.
  CHECK(HtmlToText("<p>a</p>\n\n\n\n<p>b</p>") == "a\n\nb");
  CHECK(HtmlToText("  spaced   \t out  ") == "spaced out");
  // An unterminated tag ends the document rather than leaking markup.
  CHECK(HtmlToText("visible<div class=") == "visible");
  CHECK(HtmlToText("<style>only</style>").empty());
  // A `>` inside a quoted attribute does not end the tag. Pages carry JSON in
  // attributes, and stopping early spilled the remainder out as text.
  CHECK(HtmlToText(R"(<div data-mw='{"parts":["]}'>text</div>)") == "text");
  CHECK(HtmlToText("<a href=\"?a=1&amp;b=2\">link</a>") == "link");
  // Cells are columns, not lines: without a separator the figures either side
  // of a boundary ran together into one unreadable number.
  CHECK(HtmlToText("<tr><td>US</td><td>1,000</td><td>2,000</td></tr>") ==
        "| US | 1,000 | 2,000");
  // Headings carry their depth, so the outline survives the conversion.
  CHECK(HtmlToText("<h2>Title</h2><p>body</p>") == "## Title\n\nbody");
  CHECK(HtmlToText("<h6>deep</h6>") == "###### deep");
  // Site furniture is not what a reader came for.
  CHECK(HtmlToText("<nav>menu</nav><p>real</p><footer>legal</footer>") ==
        "real");
  // Indentation inside <pre> is the meaning: collapsing it broke every
  // Python block that came back through this tool.
  CHECK(HtmlToText("<pre>def f():\n    return 1\n</pre>") ==
        "def f():\n    return 1");
  CHECK(HtmlToText("<pre><span>if x:</span>\n    pass</pre>") ==
        "if x:\n    pass");
  CHECK(HtmlToText("<p>before</p><pre>  kept  </pre><p>after</p>") ==
        "before\n\n  kept  \n\nafter");

  ChatResult result;
  StreamCtx stream;
  stream.res = &result;
  stream.HandleEvent(
      {"message",
       R"({"choices":[{"delta":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/a"}}]}}]})",
       ""});
  stream.HandleEvent(
      {"message",
       R"({"choices":[{"message":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/b"}}]}}]})",
       ""});
  stream.HandleEvent(
      {"message",
       R"({"error":{"message":"upstream overloaded","type":"server_error"}})",
       ""});
  CHECK(result.error == "upstream overloaded");
  stream.status = 200;
  stream.started = std::chrono::steady_clock::now();
  stream.last_byte = stream.started - std::chrono::seconds(1);
  auto prior_byte = stream.last_byte;
  CHECK(stream.Feed(": keepalive\n\n", 13) == 13);
  CHECK(stream.last_byte > prior_byte);
  CHECK(result.first_event_ms < 0);
  std::string citations = CitationMarkdown(result.annotations);
  CHECK(citations.find("<https://example.com/a>") != std::string::npos);
  CHECK(citations.find("<https://example.com/b>") != std::string::npos);
  auto entries = CitationEntries(result.annotations);
  CHECK(entries.size() == 2);
  SearchTrace trace;
  trace.Add(1, result.annotations);
  CHECK(trace.ArchiveMetadata().value("web_searches", int64_t{0}) == 1);
  CHECK(trace.ArchiveMetadata()["annotations"].size() == 2);
  trace.Reset();
  CHECK(trace.Empty());
  trace.Add(0, result.annotations);
  CHECK(!trace.Empty());
  json many = json::array();
  for (int i = 0; i < 25; ++i) {
    many.push_back({{"url", "https://example.com/" + std::to_string(i)},
                    {"content", std::string(5000, 'x')}});
  }
  trace.Reset();
  trace.Add(1, many);
  CHECK(trace.ArchiveMetadata()["annotations"].size() ==
        SearchTrace::kMaxSources);
  CHECK(trace.ArchiveMetadata()["annotations"][0]["content"]
            .get<std::string>()
            .size() <= SearchTrace::kMaxContentChars + 3);
  CHECK(CitationMarkdown(
            json::array({{{"url_citation", {{"url", "javascript:alert(1)"}}}}}))
            .empty());
}

}  // namespace uagent
