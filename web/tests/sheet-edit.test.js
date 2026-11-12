import test from "node:test";
import assert from "node:assert/strict";
import {
  cellOf,
  fileOf,
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

test("tabs put in or taken out are every table written anew over those the file has", () => {
  const three = `${one}\n${two}\n<table data-sheet="New">\n</table>`;
  const source = `${head}${one}\n<!-- between -->\n${two}\n<figure data-chart="bar" data-range="A1:B2"></figure>\n<p>after`;
  const all = (tabs, table) =>
    splice(source, { tab: 0, tabs, table, all: true });
  assert.equal(all(2, three), `${head}${three}\n<p>after`);
  assert.equal(all(2, two), `${head}${two}\n<p>after`);
  // The file holds other tabs than the page counted: nothing is written.
  assert.equal(all(3, three), null);
  assert.equal(all(1, one), null);
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

test("the charts right after a table are replaced with it; a figure elsewhere stays", () => {
  const chart = (type) =>
    `<figure data-chart="${type}" data-range="A1:B2"><figcaption>x</figcaption></figure>`;
  const source = `${head}${one}\n${chart("bar")}\n${chart("pie")}\n<p>text</p>\n${chart("line")}\n${two}\n<figure><img></figure>`;
  assert.equal(
    put(source, 0),
    `${head}${next}\n<p>text</p>\n${chart("line")}\n${two}\n<figure><img></figure>`,
  );
  // The other tab has none: what follows it is no chart.
  const charted = `${next}\n${chart("stacked")}`;
  assert.equal(
    splice(source, { tab: 1, tabs: 2, table: charted }),
    `${head}${one}\n${chart("bar")}\n${chart("pie")}\n<p>text</p>\n${chart("line")}\n${charted}\n<figure><img></figure>`,
  );
});

test("a chart belongs to a table only with white space between, and one left open takes nothing after it", () => {
  const chart =
    '<figure data-chart="bar" data-range="A1:B2"><figcaption>x</figcaption></figure>';
  // A comment between: the figure is not the table's, and stays.
  const apart = `${head}${one}\n<!-- costs -->\n${chart}\n${two}`;
  assert.equal(
    put(apart, 0),
    `${head}${next}\n<!-- costs -->\n${chart}\n${two}`,
  );
  // No end tag: the next tab is not taken for the chart's inside.
  const open =
    '<figure data-chart="bar" data-range="A1:B2"><figcaption>x</figcaption>';
  const unclosed = `${head}${one}\n${open}\n${two}\n<figure><img></figure>`;
  assert.equal(
    put(unclosed, 0),
    `${head}${next}\n${open}\n${two}\n<figure><img></figure>`,
  );
  assert.equal(
    put(unclosed, 1),
    `${head}${one}\n${open}\n${next}\n<figure><img></figure>`,
  );
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

test("what the page hands over is taken as the kind it names, under what a file may be", async () => {
  const bytes = new Uint8Array([80, 75, 3, 4]);
  const workbook = fileOf({ as: "xlsx", data: bytes });
  assert.deepEqual(
    [workbook.as, new Uint8Array(await workbook.blob.arrayBuffer())],
    ["xlsx", bytes],
  );
  const rows = fileOf({ as: "csv", data: "a,b" });
  assert.deepEqual([rows.as, await rows.blob.text()], ["csv", "a,b"]);
  assert.deepEqual(fileOf({ as: "sheet", data: "<table>" }), {
    as: "sheet",
    text: "<table>",
  });
  for (const said of [
    { as: "xlsx", data: "PK" },
    { as: "xlsx", data: [80, 75] },
    { as: "sheet", data: bytes },
    { as: "exe", data: "x" },
    {},
  ])
    assert.equal(fileOf(said), undefined);
  // Too large a file is not taken, and why is said; text counts in bytes.
  const exported = "Not exported: the file would be larger than 8.4 MB.";
  assert.equal(
    fileOf({ as: "xlsx", data: new Uint8Array(8 * 1024 * 1024 + 1) }),
    exported,
  );
  assert.equal(
    fileOf({ as: "csv", data: "x".repeat(8 * 1024 * 1024 + 1) }),
    exported,
  );
  const saved = "Not saved: the file would be larger than 1 MB.";
  assert.equal(fileOf({ as: "sheet", data: "é".repeat(524_289) }), saved);
  assert.equal(
    typeof fileOf({ as: "sheet", data: "é".repeat(524_288) }),
    "object",
  );
});

// `held`, when given, is what every write waits for.
test("what the page says of its selection is taken in the shape and size expected", () => {
  const summary = { count: 2, sum: 3.5, average: 1.75 };
  const marks = {
    bold: true,
    italic: false,
    wrap: false,
    align: "right",
    fill: "good",
    ink: "",
    format: "0%",
  };
  const said = {
    type: "cell",
    ref: "Costs!B2",
    raw: "=A2",
    range: "Costs!B2:C3",
    summary,
    marks,
  };
  assert.deepEqual(cellOf(said), said);
  assert.deepEqual(
    cellOf({
      ref: "a`b\nc",
      raw: 7,
      range: "x".repeat(500),
      summary: {},
      marks: { bold: "yes", fill: "a`b", format: 7, more: 1 },
      more: 1,
    }),
    {
      type: "cell",
      ref: "abc",
      raw: "7",
      range: "x".repeat(80),
      summary: undefined,
      marks: {
        ...marks,
        align: "",
        fill: "ab",
        format: "7",
      },
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
    write: async ({ path, text, revision }, closing) => {
      log.push(`write ${path} on ${revision}${closing ? ", closing" : ""}`);
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

test("a cell being typed in is held as its edit and written when the sheet is left first; an edit, or typing no more, lets it go", async () => {
  const file = { now: { text: `${head}${one}\n${two}`, revision: "r0" } };
  const { saver, log } = saving(file);
  // Nothing held: nothing is written, and all is in the file.
  assert.equal(await saver.flush(), true);
  saver.hold("a.html", edit(0, "ty"));
  saver.hold("a.html", edit(0, "typed"));
  assert.equal(await saver.flush(), true);
  assert.deepEqual(log, ["read a.html", "write a.html on r0"]);
  assert.equal(file.now.text, `${head}${edit(0, "typed").table}\n${two}`);
  // Written once: it is held no more.
  await saver.flush();
  // Typed no more, or made an edit: the edit alone is written.
  saver.hold("a.html", edit(0, "dropped"));
  saver.hold("a.html");
  await saver.flush();
  saver.hold("a.html", edit(1, "draft"));
  await saver.save("a.html", edit(1, "kept"));
  await saver.flush();
  assert.deepEqual(log.slice(2), ["write a.html on r1"]);
  assert.equal(
    file.now.text,
    `${head}${edit(0, "typed").table}\n${edit(1, "kept").table}`,
  );
  // One that cannot be written is said, and is not all in the file.
  file.now = { ...file.now, revision: "r9" };
  saver.hold("a.html", edit(0, "late"));
  assert.equal(await saver.flush(), false);
  assert.equal(log.at(-1), "failed conflict");
});

test("a closing page writes a held cell into the file as it was read while typing, and only one small enough to send", async () => {
  const file = { now: { text: `${head}${one}\n${two}`, revision: "r0" } };
  const { saver, log } = saving(file);
  saver.hold("a.html", edit(0, "typed"));
  assert.equal(await saver.flush(true), true);
  assert.deepEqual(log, ["read a.html", "write a.html on r0, closing"]);
  saver.hold("a.html", edit(0, "x".repeat(50_000)));
  assert.equal(await saver.flush(true), false);
  assert.equal(file.now.text, `${head}${edit(0, "typed").table}\n${two}`);
});
