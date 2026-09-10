import { formatBody } from "./format.ts";

// A capture viewer, not EventSource: retain comments, unknown fields and an
// unfinished final frame that a live EventSource would discard at EOF.
// Framing: https://html.spec.whatwg.org/multipage/server-sent-events.html
export function formatEventStream(source: string, readable = false) {
  const frames: string[] = [];
  let data: string[] = [],
    fields: string[] = [];
  const flush = (complete: boolean) => {
    if (!data.length && !fields.length) return;
    frames.push(
      [
        `#${frames.length + 1}${complete ? "" : " · incomplete frame"}`,
        ...fields,
        ...(data.length ? [formatBody(data.join("\n"), readable)] : []),
      ].join("\n"),
    );
    data = [];
    fields = [];
  };
  // Drop one leading BOM in the projection; Source still contains it.
  const lines = source.replace(/^\uFEFF/, "").split(/\r\n|\r|\n/);
  // split adds a sentinel after a final newline; it is not a blank SSE line.
  if (/\r$|\n$/.test(source)) lines.pop();
  for (const line of lines) {
    if (!line) {
      flush(true);
      continue;
    }
    const colon = line.indexOf(":");
    const name = colon < 0 ? line : line.slice(0, colon);
    let value = colon < 0 ? "" : line.slice(colon + 1);
    if (value.startsWith(" ")) value = value.slice(1);
    if (name === "data") data.push(value);
    else fields.push(line);
  }
  flush(false);
  return frames.join("\n\n");
}
