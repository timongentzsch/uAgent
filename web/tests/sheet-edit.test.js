import test from "node:test";
import assert from "node:assert/strict";
import {
  cellOf,
  splice,
  tableSaver,
} from "../src/features/files/sheet-edit.ts";

const head =
  '<!doctype html>\n<meta name="sheet" content="1">\n<title>Plan</title>\n';
const one =
  '<table data-sheet="One" data-frozen="1">\n<tr><th>A\n<tr><td>1\n</table>';
const two = '<table data-sheet="Two">\n<tr><td>2\n</table>';
const next = '<table data-sheet="One">\n<tr><td>9\n</table>';
const put = (source, tab, tabs = 2) =>
  splice(source, { tab, tabs, table: next });

test("only the tab's table changes: the head, the other tabs and what lies between stay byte for byte", () => {
  const between =
    '\n<!-- <table data-sheet> in a comment -->\n<figure data-chart>x</figure>\n<table data-sheet-x="1"><tr><td>not a tab</table>\n';
  const source = `${head}${one}${between}${two}\n<p>after`;
  assert.equal(put(source, 0), `${head}${next}${between}${two}\n<p>after`);
  assert.equal(put(source, 1), `${head}${one}${between}${next}\n<p>after`);
  assert.equal(put(source, 2), null);
});

test("a table's end is its own: not the text of a cell, an attribute's value or a table inside it", () => {
  const tricky =
    '<TABLE class="x" data-sheet=One>\n' +
    '<tr><td>&lt;/table&gt;<td title="</table>">quoted\n' +
    "<tr><td><table><tr><td>inner</table>\n" +
    "</Table>";
  assert.equal(put(`${head}${tricky}\n${two}`, 0), `${head}${next}\n${two}`);
  assert.equal(put(`${head}${tricky}\n${two}`, 1), `${head}${tricky}\n${next}`);
});

test("a file with CRLF line ends keeps them in the new table too", () => {
  const crlf = (text) => text.replaceAll("\n", "\r\n");
  assert.equal(
    put(crlf(`${head}${one}\n${two}\n`), 1),
    crlf(`${head}${one}\n${next}\n`),
  );
});

test("what the page says is checked: a place that is no tab, or other tabs than the page counted, changes nothing", () => {
  const source = `${head}${one}\n${two}`;
  for (const tab of [-1, null, "0", 0.5, undefined])
    assert.equal(put(source, tab), null);
  assert.equal(splice(source, { tab: 0, tabs: 2, table: null }), null);
  // A table the page does not see as a tab: the counts differ.
  const scripted = `${head}<template>${one}</template>\n${one}\n${two}`;
  assert.equal(put(scripted, 0), null);
  assert.equal(
    put(scripted, 0, 3),
    `${head}<template>${next}</template>\n${one}\n${two}`,
  );
});

// `held`, when given, is what every write waits for.
test("what the page says of its selection is taken in the shape and size expected", () => {
  const summary = { count: 2, sum: 3.5, average: 1.75 };
  const said = {
    type: "cell",
    ref: "Costs!B2",
    raw: "=A2",
    range: "Costs!B2:C3",
    summary,
  };
  assert.deepEqual(cellOf(said), said);
  assert.deepEqual(
    cellOf({
      ref: "a`b\nc",
      raw: 7,
      range: "x".repeat(500),
      summary: {},
      more: 1,
    }),
    {
      type: "cell",
      ref: "abc",
      raw: "7",
      range: "x".repeat(80),
      summary: undefined,
    },
  );
  assert.equal(
    cellOf({ summary: { ...summary, sum: "3" } }).summary,
    undefined,
  );
});

const saving = (file, held) => {
  const log = [];
  const saver = tableSaver({
    failed: (error) => log.push(`failed ${error.message}`),
    read: async (path) => {
      log.push(`read ${path}`);
      return file.now;
    },
    // As the host: a write on another revision than the file's is refused.
    write: async ({ path, text, revision }) => {
      log.push(`write ${path} on ${revision}`);
      await held;
      if (revision !== file.now.revision)
        throw Object.assign(new Error("conflict"), { status: 409 });
      file.now = { text, revision: `r${Number(revision.slice(1)) + 1}` };
      return { revision: file.now.revision };
    },
  });
  return { saver, log };
};
const edit = (tab, value) => ({
  tab,
  tabs: 2,
  table: `<table data-sheet="T${tab}">\n<tr><td>${value}\n</table>`,
});

test("edits are saved in order, one at a time, each on the revision the last left", async () => {
  const file = { now: { text: `${head}${one}\n${two}`, revision: "r0" } };
  const { saver, log } = saving(file);
  await Promise.all([
    saver.save("a.html", edit(0, 1)),
    saver.save("a.html", edit(1, 2)),
    saver.save("a.html", edit(0, 3)),
  ]);
  assert.deepEqual(log, [
    "read a.html",
    "write a.html on r0",
    "write a.html on r1",
    "write a.html on r2",
  ]);
  assert.equal(
    file.now.text,
    `${head}${edit(0, 3).table}\n${edit(1, 2).table}`,
  );
  await assert.rejects(saver.save("a.html", edit(4, 7)), /not in the file/);
});

test("a refused save is said once, the edits queued behind it are dropped, and saving goes on from the file as it is", async () => {
  const file = { now: { text: `${head}${one}\n${two}`, revision: "r0" } };
  const { saver, log } = saving(file);
  await saver.save("a.html", edit(0, 1));
  // The agent writes the file.
  file.now = { text: `${head}${one}\n${two}`, revision: "r9" };
  log.length = 0;
  const outcomes = await Promise.allSettled([
    saver.save("a.html", edit(0, 4)),
    saver.save("a.html", edit(0, 5)),
  ]);
  assert.deepEqual(
    outcomes.map((outcome) => outcome.reason?.message),
    ["conflict", "Not saved"],
  );
  assert.deepEqual(log, ["write a.html on r1", "failed conflict"]);
  assert.equal(file.now.text, `${head}${one}\n${two}`);
  await saver.save("a.html", edit(0, 6));
  assert.equal(file.now.text, `${head}${edit(0, 6).table}\n${two}`);
});

test("a sheet drawn anew while a save is on its way: that save stands, and one queued behind it is said, not dropped in silence", async () => {
  const file = { now: { text: `${head}${one}\n${two}`, revision: "r0" } };
  let release;
  const { saver, log } = saving(file, new Promise((go) => (release = go)));
  const first = saver.save("a.html", edit(0, 1));
  const second = saver.save("a.html", edit(1, 2));
  await new Promise(setImmediate);
  saver.renew();
  release();
  await first;
  await assert.rejects(second, /Not saved/);
  assert.deepEqual(log, [
    "read a.html",
    "write a.html on r0",
    "failed Not saved",
  ]);
  // The revision of the save that stood is not held: the file is read again.
  log.length = 0;
  await saver.save("a.html", edit(1, 3));
  assert.deepEqual(log, ["read a.html", "write a.html on r1"]);
});
