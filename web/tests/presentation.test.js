import { contextSummary } from "../src/context.ts";
import { duration } from "../src/duration.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { formatBody, formatJSON } from "../src/format.ts";
import { formatEventStream } from "../src/event-stream.ts";
import { count, bytes } from "../src/quantities.ts";
import { presentMessages } from "../src/message-view.ts";

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
