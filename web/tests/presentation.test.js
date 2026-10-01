import { contextSummary } from "../src/state/context.ts";
import { duration } from "../src/shared/duration.ts";
import {
  isRunningStatus,
  statusLine,
  diffLineClass,
} from "../src/shared/display.ts";
import { getToolRow } from "../src/features/chat/tool-preview.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { formatBody, formatJSON } from "../src/shared/format.ts";
import { formatEventStream } from "../src/state/event-stream.ts";
import { count, bytes } from "../src/shared/quantities.ts";
import {
  presentMessages,
  splitMentionTokens,
} from "../src/shared/message-view.ts";
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
});

test("change receipts classify git-style lines", () => {
  const change = [
    "Replaced web/src/a.ts (+2 -1)",
    " context",
    "-removed",
    "+added",
    "@line 3",
  ];
  assert.deepEqual(
    change.map((line, index) => diffLineClass(line, index === 0)),
    ["diff-head", "diff-ctx", "diff-del", "diff-add", "diff-hunk"],
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
  assert.equal(contextSummary(4600, 1300000), "est. ctx 4.6k/1.3M · 99% left");
  assert.equal(contextSummary(20469, 0), "est. ctx 20.5k");
  assert.equal(contextSummary(0, 1000), "est. ctx 0/1k · 100% left");
  assert.equal(contextSummary(2000, 1000), "est. ctx 2k/1k · 0% left");
  assert.equal(contextSummary(undefined, 1000), "est. ctx —");
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

// Pairing tests read identities; an Explored group only wraps them.
const flat = (rows) =>
  rows.flatMap((row) => (row.kind === "group" ? row.children : [row]));

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

test("tool rows read as the view's verb and target", () => {
  const base = {
    kind: "tool_result",
    name: "edit_file",
    text: "ok",
    status: "success",
  };
  assert.equal(getToolRow(base).title, "edit_file");
  assert.equal(
    getToolRow({ ...base, activity: { label: "Edited a.txt" } }).title,
    "Edited a.txt",
  );
  const view = { verb: ["Editing", "Edited"], target: "a.txt" };
  assert.equal(getToolRow({ ...base, view }, true).title, "Editing a.txt");
  const done = getToolRow({
    ...base,
    view,
    change: "Edited a.txt (+2 -1)\n@@ -1 +1 @@\n-old\n+new\n+more",
  });
  assert.equal(done.title, "Edited a.txt");
  assert.match(done.subtitle, /^\+2 \u22121 · /);
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

const call = (id, category, status = "success") => ({
  id,
  kind: "tool_result",
  call_id: id,
  status,
  activity: { category },
});

test("three or more tool calls in a row fold; a failure ends the run", () => {
  const rows = presentMessages([
    call("v1", "verify"),
    call("v2", "explore"),
    call("v3", "verify", "failed"),
    call("e1", "edit"),
    call("e2", "edit"),
    call("r1", "run"),
    { id: "m1", kind: "assistant", text: "done" },
    call("r2", "run"),
  ]);
  assert.deepEqual(
    rows.map((row) => row.children?.map((step) => step.id) || row.id),
    ["v1", "v2", "v3", ["e1", "e2", "r1"], "m1", "r2"],
  );
  // The group takes its first row's key, so the row keeps its place.
  assert.equal(rows[3].key, "e1");
});

test("a streamed row refolds only the last run", () => {
  const blocks = [
    call("a1", "run"),
    call("a2", "run"),
    call("a3", "run"),
    { id: "m1", kind: "assistant", text: "next" },
    call("b1", "edit"),
    call("b2", "edit"),
    call("b3", "edit"),
  ];
  const before = presentMessages(blocks);
  const after = presentMessages([...blocks, call("b4", "run")]);
  assert.equal(after[0], before[0]);
  assert.notEqual(after[2], before[2]);
  assert.deepEqual(
    after[2].children.map((step) => step.id),
    ["b1", "b2", "b3", "b4"],
  );
});
