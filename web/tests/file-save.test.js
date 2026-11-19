import test from "node:test";
import assert from "node:assert/strict";
import { fileSaver } from "../src/features/files/file-save.ts";

// The saver over one file kept here, which the agent may write as well.
function saverOn(text) {
  const file = { text, revision: 0 };
  const saver = fileSaver({
    read: async () => ({ text: file.text, revision: String(file.revision) }),
    write: async (_path, text, revision) => {
      if (revision !== String(file.revision)) throw new Error("refused");
      file.text = text;
      return { revision: String(++file.revision) };
    },
  });
  return {
    file,
    saver,
    agent: (text) => {
      file.text = text;
      file.revision++;
    },
  };
}

test("each change is undone and made again in turn, and a new one ends what could be made again", async () => {
  const { file, saver } = saverOn("a");
  const add = (more) => saver.write("f", (text) => text + more);
  assert.equal(await add("b"), true);
  assert.equal(await add("c"), true);
  assert.equal(await saver.write("f", (text) => text), false);
  assert.deepEqual(saver.can("f"), { undo: true, redo: false });
  assert.equal(await saver.undo("f"), true);
  assert.equal(await saver.undo("f"), true);
  assert.equal(file.text, "a");
  assert.equal(await saver.undo("f"), false);
  assert.deepEqual(saver.can("f"), { undo: false, redo: true });
  assert.equal(await saver.undo("f", true), true);
  assert.equal(file.text, "ab");
  await add("d");
  assert.equal(file.text, "abd");
  assert.deepEqual(saver.can("f"), { undo: true, redo: false });
  assert.deepEqual(saver.can("g"), { undo: false, redo: false });
});

test("a file written by someone else since is not undone, and what was written here before is forgotten", async () => {
  const { file, saver, agent } = saverOn("a");
  await saver.write("f", (text) => `${text}b`);
  agent("ab by the agent");
  await assert.rejects(saver.undo("f"), /Not undone: the file changed/);
  assert.equal(file.text, "ab by the agent");
  assert.deepEqual(saver.can("f"), { undo: false, redo: false });
  // Changed again here, only that change is undone.
  await saver.write("f", (text) => `${text}!`);
  await saver.write("f", (text) => `${text}?`);
  await saver.undo("f");
  agent("ab by the agent!!");
  await assert.rejects(saver.undo("f", true), /Not redone/);
  assert.equal(await saver.undo("f"), false);
  assert.equal(file.text, "ab by the agent!!");
});
