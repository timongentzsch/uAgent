import { useId } from "preact/hooks";
import { diffCounts, diffLineClass, formatStat } from "./display.ts";

// A scrollable region, so a keyboard can scroll it; a listener hears the
// size of the change before its lines. A tool row passes the counts its
// result states; a diff without them is counted line by line.
export default function DiffView({
  text,
  counts,
}: {
  text: string;
  counts?: [number, number];
}) {
  const lines = text.replace(/\n$/, "").split("\n");
  const summary = useId();
  return (
    <>
      <pre
        class="diff"
        role="region"
        aria-label="File changes"
        aria-describedby={summary}
        tabIndex={0}
      >
        {lines.map((line, index) => (
          <span key={index} class={diffLineClass(line, index === 0)}>
            {line === "" ? " " : line}
          </span>
        ))}
      </pre>
      <span id={summary} class="sr-only">
        {formatStat(counts || diffCounts(text)) || "No line changes"}
      </span>
    </>
  );
}
