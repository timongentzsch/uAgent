// The golden turns, as the host sends them (tests/unit/transcript_tree_test.cc
// records the same turns and checks the nodes it makes). Each turn has ids of
// its own, so turns can be put together into one conversation.

const summary = (duration_ms, extra = {}) => ({
  turn: 1,
  outcome: "complete",
  tool_calls: 0,
  direct_tool_calls: 0,
  model_calls: 1,
  direct_model_calls: 1,
  steps: 1,
  duration_ms,
  ttt_ms: 300,
  tokens_per_second: 40,
  usage: { input: 1000, output: 200, cost: 0.01 },
  ...extra,
});

const turn = (id, sequence, text) => ({
  id,
  parent: null,
  kind: "turn",
  sequence,
  time: "2026-10-28T16:00:00.000Z",
  body: { kind: "text", text },
});
const work = (turnId, state, counts, word) => ({
  id: `w-${turnId}`,
  parent: turnId,
  kind: "work",
  state,
  headline: {
    verb: state === "running" ? "Working" : "Worked",
    target: counts || "",
  },
  result: { word: word || "", unit: "step" },
});
const answer = (id, parent, sequence, text) => ({
  id,
  parent,
  kind: "message",
  role: "assistant",
  sequence,
  time: "2026-10-28T16:02:00.000Z",
  body: { kind: "markdown", text },
});
const footer = (id, parent, sequence, took, extra) => ({
  id,
  parent,
  kind: "footer",
  sequence,
  time: "2026-10-28T16:02:04.000Z",
  summary: summary(took, extra),
});
const call = (id, parent, sequence, state, verb, target, word, body) => ({
  id,
  parent,
  kind: "call",
  sequence,
  state,
  headline: { verb, target },
  ...(word && { result: { word } }),
  ...(body && { body }),
});

export const plain = [
  turn("m-2", 2, "hi"),
  answer("r-1-1-1", "m-2", 3, "Hello."),
  footer("m-4", "m-2", 4, 800),
];

export const chain = [
  turn("m-12", 12, "why does it take so long?"),
  work("m-12", "ok", "3 steps · edited 1 file", "2m 4s"),
  {
    id: "r-2-1-1:thinking",
    parent: "w-m-12",
    kind: "thinking",
    sequence: 13,
    headline: { verb: "Thinking", target: "" },
    body: { kind: "text", text: "look first" },
  },
  {
    id: "g-t-a",
    parent: "w-m-12",
    kind: "group",
    state: "ok",
    headline: { verb: "Explored", target: "2 calls" },
    result: { items: 2, unit: "call" },
  },
  call("t-a", "g-t-a", 14, "ok", "Read", "fli/search.py", "120 lines · 5ms", {
    kind: "text",
    text: "alpha",
  }),
  call("t-b", "g-t-a", 15, "ok", "Read", "fli/models.py", "88 lines · 5ms", {
    kind: "text",
    text: "beta",
  }),
  call("t-c", "w-m-12", 16, "ok", "Edited", "src/parser.ts", "+2 −1 · 5ms", {
    kind: "diff",
    text: "- const line = 0;\n+ let line = 1;\n+ const column = 0;",
  }),
  answer("r-2-2-1", "m-12", 17, "It's mostly structural."),
  footer("m-18", "m-12", 18, 124000),
];

export const failed = [
  turn("m-22", 22, "try"),
  work("m-22", "ok", "4 steps · 4 errors", "3.0s"),
  call("t-f", "w-m-22", 23, "failed", "Read", "a.txt", "failed · 5ms", {
    kind: "text",
    text: "missing",
  }),
  call("t-x", "w-m-22", 24, "cancelled", "Ran", "make", "cancelled · 5ms"),
  call("t-d", "w-m-22", 25, "failed", "Ran", "rm -rf out", "denied · 5ms"),
  // Refused before it ran: no verb, the call it would have made.
  call(
    "t-j",
    "w-m-22",
    26,
    "failed",
    "",
    "Read /etc",
    "rejected: path is outside the folder · 5ms",
    // What the model was told, which says the whole of why.
    {
      kind: "text",
      text: "error: path is outside the folder: /etc is not inside /project",
    },
  ),
  answer("r-3-2-1", "m-22", 27, "Nothing worked."),
  footer("m-28", "m-22", 28, 3000),
];

export const handed = [
  turn("m-32", 32, "make a report"),
  work("m-32", "ok", "1 step", "2.0s"),
  call("t-h", "w-m-32", 33, "ok", "Shared", "report.html", "5ms"),
  // What it handed you is the turn's, so it shows at every depth.
  {
    id: "t-h:files",
    parent: "m-32",
    kind: "message",
    role: "assistant",
    sequence: 33,
    parts: [
      {
        kind: "file",
        id: "0123456789abcdef0123456789abcdef",
        name: "report.html",
        mime: "text/html",
        bytes: 2048,
      },
    ],
  },
  answer("r-4-2-1", "m-32", 34, "Here it is."),
  footer("m-35", "m-32", 35, 2000, {
    files: [{ path: "report.html", added: 12, removed: 0, undoable: true }],
  }),
];

export const agents = [
  turn("m-42", 42, "find flights"),
  work("m-42", "ok", "2 steps", "4.0s"),
  {
    ...call(
      "t-s",
      "w-m-42",
      43,
      "ok",
      "Started",
      "agent · Find flights",
      "5ms",
    ),
    parts: [{ kind: "link", to: "agent", id: "child", label: "Open agent" }],
  },
  {
    ...call("t-r", "w-m-42", 44, "ok", "Started", "npm test", "5ms"),
    parts: [{ kind: "link", to: "activity", id: 7, label: "Open command" }],
  },
  answer("r-5-2-1", "m-42", 45, "Both are running."),
  footer("m-46", "m-42", 46, 4000),
  {
    id: "a-47",
    parent: "m-42",
    kind: "notice",
    sequence: 47,
    state: "ok",
    headline: { verb: "Finished", target: "npm test" },
    result: { word: "exit 0 · 42s" },
    body: { kind: "tail", text: "all 12 passed" },
    activity_id: 7,
  },
  {
    id: "a-48",
    parent: "m-42",
    kind: "notice",
    sequence: 48,
    state: "ok",
    routine: true,
    headline: { verb: "Saved", target: "memory note" },
  },
];

export const blankMcp = [
  turn("m-52", 52, "look it up"),
  work("m-52", "ok", "1 step", "1.5s"),
  call("t-m", "w-m-52", 53, "ok", "Called", "Search", "5ms"),
  footer("m-55", "m-52", 55, 1500),
];

export const running = [
  turn("m-62", 62, "build"),
  work("m-62", "running"),
  {
    id: "r-7-1-1",
    parent: "w-m-62",
    kind: "message",
    role: "assistant",
    sequence: 63,
    body: { kind: "markdown", text: "Building now." },
  },
  call("t-b7", "w-m-62", undefined, "running", "Running", "make"),
];

// Two pages of history: the older one is read in front of the newer.
export const pages = [plain, chain];

// A conversation with calls, a failure, a group and an answer.
export const conversation = [...chain, ...failed];
