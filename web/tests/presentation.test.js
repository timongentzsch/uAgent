import { contextSummary } from "../src/state/context.ts";
import { duration } from "../src/shared/duration.ts";
import {
  isRunningStatus,
  statusLine,
  diffLineClass,
} from "../src/shared/display.ts";
import {
  getToolPreview,
  getToolRow,
} from "../src/features/chat/tool-preview.ts";
import { liveBlocks } from "../src/state/store.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { formatBody, formatJSON } from "../src/shared/format.ts";
import { formatEventStream } from "../src/state/event-stream.ts";
import { count, bytes } from "../src/shared/quantities.ts";
import {
  presentMessages,
  splitMentionTokens,
  stripAttachedTrailer,
} from "../src/features/chat/message-view.ts";
import { dedupeName } from "../src/features/composer/mention.ts";
import {
  encodeMention,
  matchMention,
  mentionOptions,
} from "../src/features/composer/mention.ts";

test("JSON display preserves large numbers, escapes, duplicate keys and arrays", () => {
  const raw =
    '{"id":9007199254740993,"id":1e+03,"text":"a\\n\\u0061{}","empty":[],"data":[{},true,null]}';
  const formatted = formatJSON(raw);
  assert.ok(formatted.includes('"id": 9007199254740993'));
  assert.ok(formatted.includes('"id": 1e+03'));
  assert.ok(formatted.includes('"text": "a\\n\\u0061{}"'));
  assert.ok(formatted.includes('"empty": []'));
  assert.equal(formatted.match(/"id"/g).length, 2);
  assert.deepEqual(JSON.parse(formatted), JSON.parse(raw));
  assert.equal(formatBody(formatted), formatted);
});

test("SSE projection preserves frames, metadata and lexical JSON without data prefixes", () => {
  const raw =
    ': keepalive\r\nevent: delta\r\ndata: {"n":9007199254740993,\r\ndata: "s":"two"}\r\n\r\nid: 4\r\ndata: [DONE]\r\n\r\n';
  assert.equal(
    formatEventStream(raw),
    '#1\n: keepalive\nevent: delta\n{\n  "n": 9007199254740993,\n  "s": "two"\n}\n\n#2\nid: 4\n[DONE]',
  );
  assert.equal(formatBody(raw), raw);
  for (const newline of ["\n", "\r", "\r\n"]) {
    const frame = [
      "\uFEFFid: first",
      "retry: 1000",
      "event: delta",
      "ignored: kept",
      "data: one",
      "data:  two",
      "",
      "id:",
      "data",
      "",
      "data: partial",
      "",
    ].join(newline);
    assert.equal(
      formatEventStream(frame),
      "#1\nid: first\nretry: 1000\nevent: delta\nignored: kept\none\n two\n\n#2\nid:\n\n\n#3 · incomplete frame\npartial",
    );
  }
  assert.equal(
    formatEventStream("data: [DONE]\n"),
    "#1 · incomplete frame\n[DONE]",
  );
  assert.equal(formatEventStream("data: [DONE]\n\n"), "#1\n[DONE]");
  assert.equal(formatEventStream(": comment\n\n"), "#1\n: comment");
  assert.equal(formatEventStream(""), "");
  for (const text of ["", "not JSON\n", "{invalid}", "\u001b[31mraw bytes"])
    assert.equal(formatBody(text), text);
});

test("compact quantities use decimal units and promote rounded thresholds", () => {
  for (const [value, result] of [
    [0, "0"],
    [999, "999"],
    [1000, "1k"],
    [1500, "1.5k"],
    [999949, "999.9k"],
    [999950, "1M"],
    [1250000, "1.3M"],
    [1e9, "1B"],
    [1e12, "1T"],
  ])
    assert.equal(count(value), result);
  for (const value of [undefined, NaN, Infinity, -1])
    assert.equal(count(value), "Not recorded");
  for (const [value, result] of [
    [0, "0 B"],
    [999, "999 B"],
    [1000, "1 kB"],
    [1000000, "1 MB"],
    [999950, "1 MB"],
    [2411724, "2.4 MB"],
    [5 * 1024 ** 3, "5.4 GB"],
  ])
    assert.equal(bytes(value), result);
});

test("tool presentation joins parallel results by ID and keeps model metadata", () => {
  const blocks = [
    { id: "m-0", kind: "user", text: "Do work" },
    {
      id: "m-1",
      kind: "assistant",
      reply_to: "m-0",
      route: "provider/model:high",
      http: [{ id: "exchange" }],
      tools: [
        { id: "a", name: "read" },
        { id: "b", name: "run" },
      ],
    },
    {
      id: "m-2",
      kind: "tool_result",
      call_id: "b",
      text: "second",
      status: "failed",
    },
    {
      id: "m-3",
      kind: "tool_result",
      call_id: "a",
      text: "first",
      status: "success",
    },
    { id: "m-4", kind: "assistant", reply_to: "m-0", text: "Done" },
  ];
  const before = structuredClone(blocks);
  const result = presentMessages(blocks);
  assert.deepEqual(
    result.map((row) => row.text),
    ["Do work", "first", "second", "Done"],
  );
  assert.equal(result[1].source, blocks[1]);
  assert.equal(result[1].reply_to, "m-0");
  assert.equal(result[3].reply_to, "m-0");
  assert.deepEqual(blocks, before);
});

test("async receipts retain their kind with uniform chrome", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    {
      id: "m-8",
      kind: "activity",
      activity_id: 1073741824,
      activity: { category: "execute", label: "activity 1073741824" },
      text: "activity 1073741824 completed",
      status: "completed",
    },
  ]);
  assert.equal(rows.length, 2);
  assert.equal(rows[1].id, "m-8");
  assert.equal(rows[1].kind, "activity");
  assert.equal(rows[1].activity?.category, "execute");
  // No header/matrix flags: every row renders identical chrome.
  assert.ok(!("firstOfTurn" in rows[1]));
});

test("rows keep stable keys in order with no header flags", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "hi" },
    { id: "m1", kind: "assistant", text: "a" },
    { id: "t1", kind: "tool_result", call_id: "c1", text: "r" },
    { id: "m2", kind: "assistant", text: "b" },
    { id: "u2", kind: "user", text: "again" },
    { id: "t2", kind: "tool_result", call_id: "c2", text: "r2" },
  ]);
  assert.deepEqual(
    rows.map((row) => row.key || row.id),
    ["u1", "m1", "t1", "m2", "u2", "t2"],
  );
  for (const row of rows) assert.ok(!("firstOfTurn" in row));
});

test("every row renders uniform chrome: keys, kinds and order kept", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "hi" },
    { id: "m1", kind: "assistant", text: "a", turn_root: "m1" },
    {
      id: "t1",
      kind: "tool_result",
      call_id: "c1",
      text: "r",
      turn_root: "m1",
    },
    { id: "m2", kind: "assistant", text: "b", turn_root: "m1" },
    { id: "u2", kind: "user", text: "again" },
    { id: "m3", kind: "assistant", text: "c", turn_root: "m3" },
  ]);
  assert.deepEqual(
    rows.map((row) => row.id),
    ["u1", "m1", "t1", "m2", "u2", "m3"],
  );
  assert.deepEqual(
    rows.map((row) => row.kind),
    ["user", "assistant", "tool_result", "assistant", "user", "assistant"],
  );
});

test("tool-sourced attachments stay inline with uploads", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "hi" },
    { id: "a1", kind: "attachment", origin: "tool", text: "shot" },
    { id: "m1", kind: "assistant", text: "a" },
    { id: "a2", kind: "attachment", text: "mine" },
    { id: "m2", kind: "assistant", text: "b" },
  ]);
  assert.deepEqual(
    rows.map((row) => row.id),
    ["u1", "a1", "m1", "a2", "m2"],
  );
});

test("server Attached trailers strip at render, user words survive", () => {
  const files = [{ id: "f1", name: "chart.png" }];
  assert.equal(
    stripAttachedTrailer(
      'look at this\n\nAttached:\n- path "/tmp/x/805b.data"',
      files,
    ),
    "look at this",
  );
  assert.equal(
    stripAttachedTrailer(
      'a\n\nAttached:\n- path "/a/1.data" (from tool call "c1")\n- path "/b/2 \\"q\\".data"',
      files,
    ),
    "a",
  );
  // No files: a user literally typing the format keeps their words.
  assert.equal(
    stripAttachedTrailer('note\n\nAttached:\n- path "/x"', []),
    'note\n\nAttached:\n- path "/x"',
  );
  assert.equal(stripAttachedTrailer(undefined, files), undefined);
  // Mid-text lookalikes are not trailers.
  assert.equal(
    stripAttachedTrailer('x\n\nAttached:\n- path "/a"\n\nmore', files),
    'x\n\nAttached:\n- path "/a"\n\nmore',
  );
});

test("reused IDs, incomplete calls and orphaned results remain in their own turns", () => {
  const result = presentMessages([
    { id: "user-1", kind: "user" },
    {
      id: "source-1",
      kind: "assistant",
      text: "Working",
      tools: [{ id: "a", name: "run" }],
    },
    { id: "result-1", kind: "tool_result", call_id: "a", text: "first" },
    { id: "user-2", kind: "user" },
    { id: "orphan", kind: "tool_result", call_id: "a", text: "unmatched" },
    {
      id: "source-2",
      kind: "assistant",
      tools: [
        { id: "a", name: "read" },
        { id: "b", name: "pending" },
      ],
    },
    { id: "result-2", kind: "tool_result", call_id: "a", text: "second" },
  ]);
  assert.deepEqual(
    result.map((row) => row.id),
    ["user-1", "source-1", "result-1", "user-2", "orphan", "result-2", "t-b"],
  );
  assert.equal(result[2].source.id, "source-1");
  assert.equal(result[5].source.id, "source-2");
  assert.equal(result[6].result_loaded, false);
});

test("running rows never read as not recorded", () => {
  for (const status of [undefined, "", "running", "pending", "not recorded"]) {
    assert.ok(isRunningStatus(status), JSON.stringify(status));
    assert.equal(statusLine({ status }), "Running\u2026");
  }
  assert.ok(!isRunningStatus("succeeded"));
  assert.equal(
    statusLine({ category: "execute", status: "running" }),
    "execute \u00b7 Running\u2026",
  );
  assert.equal(
    statusLine({ status: undefined, duration_ms: 1500 }),
    "Done \u00b7 1.5s",
  );
  const preview = getToolPreview({
    kind: "tool_result",
    name: "run",
    status: "not recorded",
    arguments: { command: "sleep 60" },
  });
  assert.equal(preview.title, "$ sleep 60");
  assert.equal(preview.subtitle, "Running\u2026");
});

test("activity rows name the operation, including retained string arguments", () => {
  // Live blocks carry parsed objects; retained blocks carry a truncated
  // JSON string. Both must render what the call actually does.
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: { operation: "poll", id: 12 },
    }).title,
    "Poll activity 12",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: JSON.stringify({ operation: "poll", id: 12 }),
    }).title,
    "Poll activity 12",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: { operation: "poll", id: 12, until: "READY\nnoise" },
    }).title,
    "Await READY \u00b7 activity 12",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: { operation: "wait", mode: "any", ids: [1, 2] },
    }).title,
    "Wait for any \u00b7 1, 2",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: { operation: "stop", id: 7 },
    }).title,
    "Stop activity 7",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: { operation: "list" },
    }).title,
    "List activities",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: { operation: "write", id: 3, chars: "hello" },
    }).title,
    "Write 5 B \u2192 activity 3",
  );
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "activity",
      arguments: "{}",
    }).title,
    "Activity",
  );
  // String arguments rescue the other tools too.
  assert.equal(
    getToolPreview({
      kind: "tool_result",
      name: "run",
      arguments: JSON.stringify({ command: "sleep 60" }),
    }).title,
    "$ sleep 60",
  );
});

test("change receipts classify git-style lines", () => {
  const change = [
    "Replaced web/src/a.ts (+2 -1)",
    " context",
    "-removed",
    "+added",
    "@line 3",
    " … diff truncated",
  ];
  assert.deepEqual(
    change.map((line, index) => diffLineClass(line, index === 0)),
    ["diff-head", "diff-ctx", "diff-del", "diff-add", "diff-hunk", "diff-cut"],
  );
  assert.equal(diffLineClass("@@ -1,3 +1,4 @@", false), "diff-hunk");
  assert.equal(diffLineClass("--- a/f", false), "diff-head");
  assert.equal(diffLineClass("+++ b/f", false), "diff-head");
  assert.equal(diffLineClass("+x", false), "diff-add");
});

test("durations scale consistently with the CLI", () => {
  for (const [value, expected] of [
    [0, "0ms"],
    [840, "840ms"],
    [2440, "2.4s"],
    [60000, "1m"],
    [2461000, "41m 1s"],
    [4320000, "1h 12m"],
    [183600000, "2d 3h"],
  ])
    assert.equal(duration(value), expected);
  assert.equal(duration(undefined), "Not recorded");
});

test("context headroom matches CLI rounding and handles unknown and exceeded limits", () => {
  assert.equal(contextSummary(4600, 1300000), "ctx 4.6k/1.3M · 99% left");
  assert.equal(contextSummary(20469, 0), "ctx 20.5k");
  assert.equal(contextSummary(0, 1000), "ctx 0/1k · 100% left");
  assert.equal(contextSummary(2000, 1000), "ctx 2k/1k · 0% left");
  assert.equal(contextSummary(undefined, 1000), "ctx —");
});

test("readable bodies decode text and nested JSON without losing lexical facts", () => {
  const payload = {
    messages: [{ content: 'First line\nSecond line\t"quoted"' }],
    tool_calls: [
      {
        function: {
          arguments: JSON.stringify({
            pattern: "\\bword\\b",
            code: 'print("hello")\nnext()',
          }),
        },
      },
    ],
    plain: "{not JSON}\nC:\\workspace",
    control: "\u001b[31mvisible escape",
  };
  const source =
    '{"id":9007199254740993,"id":1e+03,' + JSON.stringify(payload).slice(1);
  const readable = formatBody(source, true);
  assert.ok(readable.includes('First line\n      Second line\t"quoted"'));
  assert.ok(readable.includes('"pattern": "\\bword\\b"'));
  assert.ok(readable.includes('print("hello")\n'));
  assert.ok(readable.includes("C:\\workspace"));
  assert.ok(readable.includes("\\u001b[31mvisible escape"));
  assert.ok(readable.includes('"id": 9007199254740993'));
  assert.ok(readable.includes('"id": 1e+03'));
  assert.equal(readable.match(/"id"/g).length, 2);
  assert.equal(formatBody("{invalid}\ntext", true), "{invalid}\ntext");
  assert.deepEqual(JSON.parse(formatJSON(source)), JSON.parse(source));
  assert.ok(
    formatEventStream(`data: ${JSON.stringify(payload)}\n\n`, true).includes(
      "First line\n      Second line",
    ),
  );
});

test("reversed tool results join by ID as flat rows", () => {
  const group = { id: "a", label: "Explored · 2 calls" };
  const blocks = [
    { id: "request", kind: "user", text: "Inspect" },
    {
      id: "calls",
      kind: "assistant",
      tools: [
        { id: "a", name: "read_path" },
        { id: "b", name: "run" },
      ],
    },
    {
      id: "result-b",
      kind: "tool_result",
      call_id: "b",
      text: "B",
      activity: { category: "explore", group },
    },
    {
      id: "result-a",
      kind: "tool_result",
      call_id: "a",
      text: "A",
      activity: { category: "explore", group },
    },
    { id: "answer", kind: "assistant", text: "Done" },
    { id: "failed", kind: "tool_result", status: "failed", text: "error" },
  ];
  const rows = presentMessages(blocks);
  assert.deepEqual(
    rows.map((row) => row.id),
    ["request", "result-a", "result-b", "answer", "failed"],
  );
  assert.deepEqual(
    rows.slice(1, 3).map((row) => row.text),
    ["A", "B"],
  );
  assert.equal(rows[2].name, "run");
  assert.deepEqual(presentMessages(structuredClone(blocks)), rows);
  assert.ok(!blocks[1].children);
});

test("occurrence identities isolate repeated provider call IDs", () => {
  const group = { id: "explore", label: "Explored 2 files" };
  const blocks = [
    { id: "u-1", kind: "user", text: "first" },
    {
      id: "m-1",
      response_id: "response-1",
      kind: "assistant",
      tools: [
        {
          call_id: "same",
          occurrence_id: "response-1:1",
          response_id: "response-1",
          name: "read",
          activity: { category: "explore", group },
        },
        {
          call_id: "next",
          occurrence_id: "response-1:2",
          response_id: "response-1",
          name: "read",
          activity: { category: "explore", group },
        },
      ],
    },
    {
      id: "r-2",
      kind: "tool_result",
      call_id: "next",
      occurrence_id: "response-1:2",
      response_id: "response-1",
      text: "second",
      activity: { category: "explore", group },
    },
    {
      id: "r-1",
      kind: "tool_result",
      call_id: "same",
      occurrence_id: "response-1:1",
      response_id: "response-1",
      text: "first",
      activity: { category: "explore", group },
    },
    { id: "u-2", kind: "user", text: "again" },
    {
      id: "m-2",
      response_id: "response-2",
      kind: "assistant",
      tools: [
        {
          call_id: "same",
          occurrence_id: "response-2:1",
          response_id: "response-2",
          name: "read",
        },
      ],
    },
    {
      id: "r-3",
      kind: "tool_result",
      call_id: "same",
      occurrence_id: "response-2:1",
      response_id: "response-2",
      text: "later",
    },
  ];
  const first = presentMessages(blocks);
  assert.deepEqual(
    first.map((row) => [row.key, row.text]),
    [
      ["u-1", "first"],
      ["response-1:1", "first"],
      ["response-1:2", "second"],
      ["u-2", "again"],
      ["response-2:1", "later"],
    ],
  );
  assert.equal(first.at(-1).key, "response-2:1");
  assert.equal(first.at(-1).text, "later");
  const checkpoint = presentMessages(structuredClone(blocks));
  assert.deepEqual(
    checkpoint.map((row) => row.key),
    first.map((row) => row.key),
  );
});

test("empty assistant placeholders drop, content stays", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    { id: "r-empty", kind: "assistant", text: "", reasoning: "" },
    { id: "r-stream", kind: "assistant", streaming: true },
    { id: "m1", kind: "assistant", text: "working" },
  ]);
  assert.deepEqual(
    rows.map((row) => row.id),
    ["u1", "m1"],
  );
});

test("upload names dedupe with numbered copies", () => {
  assert.equal(dedupeName("image.png", []), "image.png");
  assert.equal(dedupeName("image.png", ["image.png"]), "image (2).png");
  assert.equal(
    dedupeName("image.png", ["image.png", "image (2).png"]),
    "image (3).png",
  );
  assert.equal(dedupeName("README", ["README"]), "README (2)");
  assert.equal(dedupeName("  ", []), "attachment");
});

test("@-mention matches the caret word, never emails", () => {
  assert.deepEqual(matchMention("see @cha", 8), {
    start: 4,
    end: 8,
    query: "cha",
  });
  assert.deepEqual(matchMention("@", 1), { start: 0, end: 1, query: "" });
  assert.equal(matchMention("a@b", 3), null);
  assert.equal(matchMention("mail x@y", 8), null);
  assert.equal(matchMention("see chart", 9), null);
  assert.deepEqual(matchMention("@a @b", 5), {
    start: 3,
    end: 5,
    query: "b",
  });
  const files = [{ name: "chart.png" }, { name: "photo.png" }];
  assert.deepEqual(mentionOptions(files, "ch"), [{ name: "chart.png" }]);
  assert.deepEqual(mentionOptions(files, ""), files);
  assert.equal(encodeMention("a]b(c", "f1"), "![a b c](attachment:f1)");
});

test("mention tokens split outside fences, never without files", () => {
  const files = [{ id: "f1" }];
  assert.deepEqual(splitMentionTokens("see ![c](attachment:f1) ok", files), [
    { text: "see " },
    { mention: { id: "f1", alt: "c" } },
    { text: " ok" },
  ]);
  assert.deepEqual(splitMentionTokens("plain", files), [{ text: "plain" }]);
  // No files: still splits — MentionFile degrades the dangling id.
  assert.deepEqual(splitMentionTokens("a ![c](attachment:f1)"), [
    { text: "a " },
    { mention: { id: "f1", alt: "c" } },
  ]);
  assert.deepEqual(
    splitMentionTokens(
      "```\n![c](attachment:f1)\n```\nreal ![d](attachment:f2)",
      files,
    ),
    [
      { text: "```\n![c](attachment:f1)\n```\nreal " },
      { mention: { id: "f2", alt: "d" } },
    ],
  );
});

test("live call and result frames merge into one completed row", () => {
  // Journal-faithful slim frames: occurrence only, no arguments/activity.
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    {
      id: "r-3-41-1:c77f",
      kind: "tool_result",
      call_id: undefined,
      occurrence_id: "r-3-41-1:c77f",
      name: "run",
      status: "running",
    },
    {
      id: "r-3-41-1:c77f",
      kind: "tool_result",
      occurrence_id: "r-3-41-1:c77f",
      name: "run",
      status: "success",
      duration_ms: 807,
    },
  ]);
  assert.equal(rows.length, 2);
  assert.equal(rows[1].status, "success");
  assert.equal(rows[1].duration_ms, 807);
});

test("retained call and result join in either order", () => {
  const call = {
    id: "m-1",
    kind: "assistant",
    response_id: "r-3-41-1",
    tools: [
      {
        call_id: "call_abc",
        occurrence_id: "r-3-41-1:c77f",
        response_id: "r-3-41-1",
        detail_id: "t-deadbeef",
        name: "run",
        arguments: '{"command":"ssh dev"}',
        activity: { category: "execute", label: "$ ssh dev" },
        status: "running",
      },
    ],
  };
  const result = {
    id: "m-2",
    kind: "tool_result",
    call_id: "call_abc",
    occurrence_id: "r-3-41-1:c77f",
    response_id: "r-3-41-1",
    detail_id: "t-deadbeef",
    name: "run",
    text: "OK",
    status: "success",
    duration_ms: 807,
  };
  for (const blocks of [
    [{ id: "u1", kind: "user", text: "go" }, call, result],
    [{ id: "u1", kind: "user", text: "go" }, result, call],
  ]) {
    const rows = presentMessages(structuredClone(blocks));
    assert.equal(rows.length, 2);
    assert.equal(rows[1].status, "success");
    assert.equal(rows[1].duration_ms, 807);
    assert.equal(rows[1].arguments, '{"command":"ssh dev"}');
    assert.equal(rows[1].activity?.label, "$ ssh dev");
  }
});

test("results without occurrence rejoin by response/call and detail", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    {
      id: "m-1",
      kind: "assistant",
      response_id: "r-1",
      tools: [
        {
          call_id: "call_old",
          response_id: "r-1",
          detail_id: "t-aaa",
          name: "run",
          arguments: '{"command":"ls"}',
          status: "running",
        },
      ],
    },
    {
      id: "m-2",
      kind: "tool_result",
      call_id: "call_old",
      response_id: "r-1",
      detail_id: "t-aaa",
      name: "run",
      text: "out",
      status: "success",
      duration_ms: 12,
    },
  ]);
  assert.equal(rows.length, 2);
  assert.equal(rows[1].text, "out");
  assert.equal(rows[1].arguments, '{"command":"ls"}');
});

test("completed rows never regress to stale Running arrivals", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    {
      id: "m-2",
      kind: "tool_result",
      occurrence_id: "r-1:abc",
      name: "run",
      text: "done",
      status: "success",
      duration_ms: 45,
    },
    {
      id: "m-1",
      kind: "assistant",
      response_id: "r-1",
      tools: [
        {
          call_id: "c1",
          occurrence_id: "r-1:abc",
          response_id: "r-1",
          name: "run",
          status: "running",
        },
      ],
    },
  ]);
  assert.equal(rows.length, 2);
  assert.equal(rows[1].status, "success");
  assert.equal(rows[1].duration_ms, 45);
  assert.equal(rows[1].text, "done");
});

test("absent fields never wipe present ones across paths", () => {
  // Slim result frame (no activity/arguments, like the journal) must
  // not clear the call record it lands on.
  const live = liveBlocks([
    {
      type: "tool.call",
      data: {
        occurrence_id: "r-1:abc",
        name: "run",
        activity: { category: "execute", label: "$ ls" },
        arguments: '{"command":"ls"}',
      },
    },
    {
      type: "tool.result",
      data: {
        occurrence_id: "r-1:abc",
        name: "run",
        completion_status: "success",
        duration_ms: 12,
      },
    },
  ]);
  assert.equal(live.length, 1);
  assert.equal(live[0].activity?.label, "$ ls");
  assert.equal(live[0].arguments, '{"command":"ls"}');
  assert.equal(live[0].status, "success");
  // Same guarantee in the retained join: a bare duplicate carries no
  // activity, so the merged row keeps the call's.
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    ...live,
    {
      id: "m-2",
      kind: "tool_result",
      occurrence_id: "r-1:abc",
      name: "run",
      status: "success",
      duration_ms: 12,
    },
  ]);
  assert.equal(rows.length, 2);
  assert.equal(rows[1].activity?.label, "$ ls");
  assert.equal(rows[1].status, "success");
});

test("server replay owns the tool title when present", () => {
  const base = {
    kind: "tool_result",
    name: "read_path",
    arguments: { path: "a.txt" },
    text: "local first line\nsecond",
    status: "success",
  };
  const local = getToolRow(base);
  assert.equal(local.server, false);
  assert.equal(local.title, "Read a.txt");
  const server = getToolRow({
    ...base,
    replay: { title: "[2] read_path", summary: "a.txt · +3 lines" },
  });
  assert.equal(server.server, true);
  assert.equal(server.title, "[2] read_path");
  assert.equal(server.preview, "a.txt · +3 lines");
});

test("consecutive tools stay flat rows in order", () => {
  const rows = presentMessages([
    { id: "u1", kind: "user", text: "go" },
    { id: "t1", kind: "tool_result", call_id: "c1", text: "one" },
    { id: "t2", kind: "tool_result", call_id: "c2", text: "two" },
    {
      id: "a1",
      kind: "attachment",
      origin: "tool",
      files: [{ id: "f1" }],
    },
    { id: "m1", kind: "assistant", text: "done" },
    { id: "t3", kind: "tool_result", call_id: "c3", text: "solo" },
    { id: "m2", kind: "assistant", text: "end" },
  ]);
  assert.deepEqual(
    rows.map((row) => row.id),
    ["u1", "t1", "t2", "a1", "m1", "t3", "m2"],
  );
});
