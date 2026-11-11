// A sheet's edited tab saved into its file: only that tab's table changes
// in the source text, so what the agent wrote around it stays as it is.

import type { PageMessage } from "../../shared/page-frame.tsx";

export type Cell = Extract<PageMessage, { type: "cell" }>;

// The page is untrusted: a name goes into the draft, so it is one short
// line without backticks, and a summary counts only as three numbers.
const name = (said: unknown) =>
  String(said ?? "")
    .replace(/[`\r\n]/g, "")
    .slice(0, 80);
export function cellOf(said: Cell): Cell {
  const { count, sum, average } = said.summary ?? {};
  return {
    type: "cell",
    ref: name(said.ref),
    raw: String(said.raw ?? "").slice(0, 8192),
    range: name(said.range),
    summary:
      Number.isFinite(count) && Number.isFinite(sum) && Number.isFinite(average)
        ? { count: count!, sum: sum!, average: average! }
        : undefined,
  };
}

// What the sheet's page says of a changed tab: its place among how many
// tabs, and its table. The page is untrusted, so each is checked.
export interface TableEdit {
  tab: number;
  tabs: number;
  table: string;
}

// A comment, or a tag with its quoted attribute values.
const TAG =
  /<!--[\s\S]*?-->|<(\/?)([a-zA-Z][\w-]*)((?:"[^"]*"|'[^']*'|[^>"'])*)>/g;

// `source` with the edit's tab replaced, in the file's own line ends. Null
// when the source does not hold the tabs the page counted: then its tables
// are not the page's, and the place would be a guess.
export function splice(source: string, { tab, tabs, table }: TableEdit) {
  if (!Number.isInteger(tab) || tab < 0 || typeof table !== "string")
    return null;
  let seen = 0;
  let depth = 0;
  let cut: [number, number] | undefined;
  let start = -1;
  for (const tag of source.matchAll(TAG)) {
    if (tag[2]?.toLowerCase() !== "table") continue;
    const opens = !tag[1];
    if (opens && /\sdata-sheet(?![\w-])/i.test(tag[3]) && seen++ === tab)
      start = tag.index;
    if (start < 0 || cut) continue;
    depth += opens ? 1 : -1;
    if (!depth) cut = [start, tag.index + tag[0].length];
  }
  if (!cut || seen !== tabs) return null;
  return (
    source.slice(0, cut[0]) +
    (source.includes("\r\n") ? table.replaceAll("\n", "\r\n") : table) +
    source.slice(cut[1])
  );
}

interface Held {
  path: string;
  text: string;
  revision: string;
}

// Saves edits in order, one write at a time, each on the revision the last
// left. One that fails is said to `failed` once and not retried; edits made
// before the sheet was read anew are dropped, not written over the file.
export function tableSaver(io: {
  read: (path: string) => Promise<{ text: string; revision?: string }>;
  write: (held: Held) => Promise<{ revision: string }>;
  failed: (error: unknown, path: string) => void;
}) {
  let held: Held | undefined;
  let round = 0;
  let said = -1;
  let last: Promise<void> = Promise.resolve();
  const renew = () => {
    held = undefined;
    round++;
  };
  const write = async (of: number, path: string, edit: TableEdit) => {
    const current = () => {
      if (of !== round) throw new Error("Not saved");
    };
    try {
      current();
      if (held?.path !== path) {
        const { text, revision } = await io.read(path);
        current();
        held = { path, text, revision: revision ?? "" };
      }
      const text = splice(held.text, edit);
      if (text === null) throw new Error("The tab is not in the file.");
      const next = { path, text, revision: held.revision };
      const { revision } = await io.write(next);
      if (of === round) held = { ...next, revision };
    } catch (error) {
      if (of === round) renew();
      if (said !== of) io.failed(error, path);
      said = of;
      throw error;
    }
  };
  return {
    save(path: string, edit: TableEdit) {
      const run = write.bind(null, round, path, edit);
      last = last.then(run, run);
      return last;
    },
    // For a sheet drawn anew from its file: edits made before are not saved.
    renew,
  };
}
