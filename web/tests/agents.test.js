import { test } from "node:test";
import assert from "node:assert/strict";
import { agentReports } from "../src/shared/message-view.ts";
import { agentGroups, atWork, statusWord } from "../src/state/attention.ts";

test("one word for where an agent is", () => {
  const word = (item) => statusWord({ id: "a", ...item });
  assert.equal(word({ needs_you: true, status: "waiting" }), "Needs you");
  assert.equal(word({ status: "waiting", turn_active: true }), "Waiting");
  assert.equal(word({ status: "starting" }), "Starting");
  assert.equal(word({ turn_active: true }), "Working");
  assert.equal(word({ status: "interrupted" }), "Interrupted");
  assert.equal(word({ error: "x" }), "Failed");
  assert.equal(word({ status: "idle" }), "Finished");
  assert.equal(atWork({ id: "a", status: "waiting" }), true);
  assert.equal(atWork({ id: "a", status: "idle" }), false);
});

test("agents group by the call that started them, newest group first", () => {
  const agent = (id, agent_call, fields) => ({ id, agent_call, ...fields });
  const groups = agentGroups([
    agent("a", "old", { updated: 10 }),
    agent("b", "new", { turn_started_ms: 500 }),
    agent("c", "new", { turn_started_ms: 400, updated: 900 }),
    agent("d", undefined, { updated: 100 }),
  ]);
  assert.deepEqual(
    groups.map((group) => group.agents.map((item) => item.id)),
    [["b", "c"], ["d"], ["a"]],
  );
  assert.equal(groups[0].at, 400);
});

test("the latest report of each agent is its first line", () => {
  const node = (session, outcome, text) => ({
    origin: "agent",
    agent: { session, title: "x", outcome },
    body: { text },
  });
  assert.deepEqual(
    agentReports([
      node("a", "completed", "Old.\nMore"),
      node("a", "completed", "New.\nMore"),
      node("b", "needs decision", "Run it?"),
      node("c", "merged", "Side"),
      { origin: "message", agent: { session: "d", outcome: "note" } },
    ]),
    { a: "New." },
  );
});
