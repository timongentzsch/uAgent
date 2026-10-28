import { contextSummary } from "../src/state/context-summary.ts";
import { duration } from "../src/shared/duration.ts";
import {
  diffLineClass,
  servedBy,
  servedByRows,
} from "../src/shared/display.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { formatBody, formatJSON } from "../src/shared/format.ts";
import { formatEventStream } from "../src/features/chat/raw-capture.ts";
import { count, bytes, tokens } from "../src/shared/quantities.ts";
import { splitMentionTokens } from "../src/shared/message-view.ts";
import {
  currentStep,
  depthOf,
  fold,
  grow,
  keepFirst,
  headlineOf,
  isRow,
  layout,
  mergeNodes,
  opens,
  placeNode,
  visible,
} from "../src/shared/tree.ts";
import * as golden from "./golden-turns.js";
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

test("a turn's tokens add every disjoint usage bucket", () => {
  // Live on Claude: 4 uncached + 17,600 cache write + 196 out, once shown as 200.
  assert.equal(
    tokens({
      input: 4,
      cache_read: 0,
      cache_write: 17600,
      output: 196,
      cost: 0,
    }),
    17800,
  );
  assert.equal(tokens({ input: 3, reasoning: 2, cost: 0 }), 5);
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

test("changes classify git-style lines", () => {
  const change = [" context", "-removed", "+added", "@line 3"];
  assert.deepEqual(change.map(diffLineClass), [
    "diff-ctx",
    "diff-del",
    "diff-add",
    "diff-hunk",
  ]);
  assert.equal(diffLineClass("@@ -1,3 +1,4 @@"), "diff-hunk");
  assert.equal(diffLineClass("--- a/f"), "diff-head");
  assert.equal(diffLineClass("+++ b/f"), "diff-head");
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
  assert.equal(contextSummary(4600, 1300000, true), "Context 99% left");
  assert.equal(contextSummary(20469, 0, true), "Context 20.5k tokens");
  assert.equal(contextSummary(2000, 1000, true), "Context 0% left");
  assert.equal(contextSummary(undefined, 1000, true), "Context —");
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

test("call details and statistics name the upstream that served each call", () => {
  assert.equal(servedBy("Wafer"), "served by Wafer");
  assert.equal(servedBy(), "");
  assert.deepEqual(
    servedByRows({
      Wafer: { model_calls: 3, rejected_calls: 1 },
      DigitalOcean: { model_calls: 1 },
    }),
    [
      ["Served by Wafer", "3 model calls · 1 rejected tool call"],
      ["Served by DigitalOcean", "1 model call"],
    ],
  );
  assert.deepEqual(servedByRows(), []);
});

// What a fresh transcript draws of `nodes` at a detail level, one line per
// thing drawn, as node.tsx lays it out: a row as its headline and result
// word (⌄ open, › closed, · nothing to open), its open body as "| text" and
// its parts in brackets, a message as its text ("you: " for yours), with
// what opens under a row indented beneath it.
function outline(nodes, detail, max = Infinity) {
  const level = depthOf(detail);
  const lines = [];
  const walk = (branches, depth, indent) => {
    for (const { branch } of layout(branches, level)) {
      const { node } = branch;
      if (!isRow(node)) {
        lines.push(
          indent +
            (node.kind === "footer"
              ? "footer"
              : `${node.role === "assistant" ? "" : "you: "}${
                  node.body?.text ??
                  (node.parts || [])
                    .map((part) => `[${part.kind} ${part.name}]`)
                    .join(" ")
                }`),
        );
        continue;
      }
      const kids = branch.kids.filter((kid) => visible(kid.node, level));
      const openable =
        node.body || kids.length || node.parts || node.kind === "call";
      const open = openable && opens(depth, level);
      const word = node.result?.word ? ` · ${node.result.word}` : "";
      const mark = !openable ? "·" : open ? "⌄" : "›";
      lines.push(`${indent}${mark} ${headlineOf(node)}${word}`);
      if (!open) continue;
      if (node.body) lines.push(`${indent}  | ${node.body.text}`);
      for (const part of node.parts || [])
        lines.push(`${indent}  [${part.kind} ${part.name ?? part.label}]`);
      const [earlier, shown] = fold(kids, max);
      if (earlier.length) lines.push(`${indent}  … ${earlier.length} earlier`);
      walk(shown, depth + 1, `${indent}  `);
    }
  };
  walk(grow(nodes), 0, "");
  return lines;
}

// Each golden turn at the three depths: minimal, default and full.
const depths = (nodes) =>
  Object.fromEntries(
    ["minimal", "default", "full"].map((level) => [
      level,
      outline(nodes, level),
    ]),
  );

test("a plain answer is the same at every depth", () => {
  const { minimal, default: middle, full } = depths(golden.plain);
  for (const lines of [minimal, middle, full])
    assert.deepEqual(lines, ["you: hi", "Hello.", "footer"]);
});

test("a chain of calls opens one level per depth", () => {
  const { minimal, default: middle, full } = depths(golden.chain);
  const ask = "you: why does it take so long?";
  const worked = "Worked 3 steps · edited 1 file · 2m 4s";
  const end = ["It's mostly structural.", "footer"];
  assert.deepEqual(minimal, [ask, `› ${worked}`, ...end]);
  assert.deepEqual(middle, [
    ask,
    `⌄ ${worked}`,
    "  › Thinking",
    "  › Explored 2 calls",
    "  › Edited src/parser.ts · +2 −1 · 5ms",
    ...end,
  ]);
  assert.deepEqual(full, [
    ask,
    `⌄ ${worked}`,
    "  ⌄ Thinking",
    "    | look first",
    "  ⌄ Explored 2 calls",
    "    ⌄ Read fli/search.py · 120 lines · 5ms",
    "      | alpha",
    "    ⌄ Read fli/models.py · 88 lines · 5ms",
    "      | beta",
    "  ⌄ Edited src/parser.ts · +2 −1 · 5ms",
    "    | - const line = 0;\n+ let line = 1;\n+ const column = 0;",
    ...end,
  ]);
});

test("failed, cancelled, denied and rejected calls read as their words", () => {
  const { minimal, default: middle, full } = depths(golden.failed);
  const end = ["Nothing worked.", "footer"];
  const worked = "Worked 4 steps · 4 errors · 3.0s";
  assert.deepEqual(minimal, ["you: try", `› ${worked}`, ...end]);
  const calls = [
    "Read a.txt · failed · 5ms",
    "Ran make · cancelled · 5ms",
    "Ran rm -rf out · denied · 5ms",
    // Refused before it ran: no verb, the call it would have made.
    "Read /etc · rejected: path is outside the folder · 5ms",
  ];
  assert.deepEqual(middle, [
    "you: try",
    `⌄ ${worked}`,
    ...calls.map((call) => `  › ${call}`),
    ...end,
  ]);
  assert.deepEqual(full, [
    "you: try",
    `⌄ ${worked}`,
    `  ⌄ ${calls[0]}`,
    "    | missing",
    ...calls.slice(1, 3).map((call) => `  ⌄ ${call}`),
    `  ⌄ ${calls[3]}`,
    "    | error: path is outside the folder: /etc is not inside /project",
    ...end,
  ]);
});

test("a file handed over stays in sight at every depth", () => {
  const { minimal, default: middle, full } = depths(golden.handed);
  const end = ["[file report.html]", "Here it is.", "footer"];
  assert.deepEqual(minimal, [
    "you: make a report",
    "› Worked 1 step · 2.0s",
    ...end,
  ]);
  assert.deepEqual(middle, [
    "you: make a report",
    "⌄ Worked 1 step · 2.0s",
    "  › Shared report.html · 5ms",
    ...end,
  ]);
  assert.equal(full[2], "  ⌄ Shared report.html · 5ms");
  assert.deepEqual(full.slice(3), end);
});

test("agents and background work link to it, and routine notices wait for full", () => {
  const { minimal, default: middle, full } = depths(golden.agents);
  assert.deepEqual(minimal, [
    "you: find flights",
    "› Worked 2 steps · 4.0s",
    "Both are running.",
    "footer",
    "› Finished npm test · exit 0 · 42s",
  ]);
  assert.deepEqual(middle, [
    "you: find flights",
    "⌄ Worked 2 steps · 4.0s",
    "  › Started agent · Find flights · 5ms",
    "  › Started npm test · 5ms",
    "Both are running.",
    "footer",
    "⌄ Finished npm test · exit 0 · 42s",
    "  | all 12 passed",
  ]);
  assert.deepEqual(full.slice(2, 6), [
    "  ⌄ Started agent · Find flights · 5ms",
    "    [link Open agent]",
    "  ⌄ Started npm test · 5ms",
    "    [link Open command]",
  ]);
  assert.equal(full.at(-1), "· Saved memory note");
});

test("a blank reply is no node, and a call without facts reads its state", () => {
  const { minimal, default: middle, full } = depths(golden.blankMcp);
  assert.deepEqual(minimal, [
    "you: look it up",
    "› Worked 1 step · 1.5s",
    "footer",
  ]);
  assert.equal(middle[2], "  › Called Search · 5ms");
  assert.equal(full[2], "  ⌄ Called Search · 5ms");
});

test("a running turn's work line says what it is doing now", () => {
  const { minimal, default: middle, full } = depths(golden.running);
  assert.deepEqual(minimal, ["you: build", "› Working"]);
  assert.deepEqual(middle, [
    "you: build",
    "⌄ Working",
    "  Building now.",
    "  › Running make",
  ]);
  assert.equal(full.at(-1), "  ⌄ Running make");
  const [turn] = grow(golden.running);
  assert.equal(currentStep(turn.kids[0]), "Running make");
});

test("paged history reads older pages in front, the newer copy winning", () => {
  const [older, newer] = golden.pages;
  const merged = mergeNodes(older, newer);
  assert.deepEqual(outline(merged, "minimal"), [
    ...outline(older, "minimal"),
    ...outline(newer, "minimal"),
  ]);
  const restated = { ...older[1], body: { kind: "markdown", text: "Hi!" } };
  const again = mergeNodes(older, [restated, ...newer]);
  assert.equal(again.length, merged.length);
  assert.equal(
    again.find((node) => node.id === restated.id),
    restated,
  );
  assert.equal(mergeNodes([], newer), newer);
});

test("a long list of steps shows its latest and folds the earlier", () => {
  const steps = Array.from({ length: 35 }, (_, index) => ({
    id: `t-${index}`,
    parent: "w-m-2",
    kind: "call",
    state: "ok",
    sequence: 3 + index,
    headline: { verb: "Read", target: `f${index}` },
  }));
  const nodes = [
    golden.plain[0],
    { ...golden.chain[1], id: "w-m-2", parent: "m-2" },
    ...steps,
  ];
  const lines = outline(nodes, "default", 30);
  assert.equal(lines[2], "  … 5 earlier");
  assert.equal(lines[3], "  › Read f5");
  assert.equal(lines.length, 3 + 30);
});

test("a node lands where the host puts it", () => {
  const patch = (block) => ({ kind: "block", block });
  const turn = { id: "m-1", parent: null, kind: "turn", sequence: 1 };
  const call = { id: "t-a", parent: "w-m-1", kind: "call", sequence: 2 };
  let nodes = placeNode([], patch(turn));
  nodes = placeNode(nodes, patch(call));
  // A parent sent after a node under it goes before it.
  nodes = placeNode(nodes, patch({ id: "w-m-1", parent: "m-1", kind: "work" }));
  assert.deepEqual(
    nodes.map((node) => node.id),
    ["m-1", "w-m-1", "t-a"],
  );
  // A late saved node goes in sequence order.
  nodes = placeNode(nodes, patch({ id: "m-0", parent: null, sequence: 0 }));
  assert.equal(nodes[0].id, "m-0");
  // Streamed text appends to the node it names; an unknown one is dropped.
  const reply = {
    id: "r",
    parent: "m-1",
    kind: "message",
    body: { kind: "markdown", text: "Hel" },
  };
  nodes = placeNode(nodes, patch(reply));
  nodes = placeNode(nodes, { kind: "block", id: "r", append: { text: "lo" } });
  assert.equal(nodes.at(-1).body.text, "Hello");
  assert.equal(
    placeNode(nodes, { kind: "block", id: "x", append: { text: "!" } }),
    nodes,
  );
});

test("a changed node makes anew only the branches on its path", () => {
  const nodes = golden.conversation;
  const before = grow(nodes);
  const at = nodes.findIndex((node) => node.id === "r-3-2-1");
  const changed = nodes.with(at, {
    ...nodes[at],
    body: { kind: "markdown", text: "Nothing worked at all." },
  });
  const after = grow(changed);
  // The other turn, and the changed turn's work and footer, are the same.
  assert.equal(after[0], before[0]);
  assert.notEqual(after[1], before[1]);
  assert.equal(after[1].kids[0], before[1].kids[0]);
  assert.notEqual(after[1].kids[1], before[1].kids[1]);
  assert.equal(after[1].kids[2], before[1].kids[2]);
});

test("older history is bounded on whole turns", () => {
  const nodes = mergeNodes(golden.plain, golden.chain);
  // Cut inside the second turn: it goes whole, not without its end.
  assert.deepEqual(keepFirst(nodes, 5), golden.plain);
  assert.equal(keepFirst(nodes, nodes.length), nodes);
});
