import { useId } from "preact/hooks";
import { diffLineClass, formatStat } from "./display.ts";

// A scrollable region, so a keyboard can scroll it; a listener hears the
// size of the change before its lines when the counts are known.
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
        aria-describedby={counts && summary}
        tabIndex={0}
      >
        {lines.map((line, index) => (
          <span key={index} class={diffLineClass(line)}>
            {line === "" ? " " : line}
          </span>
        ))}
      </pre>
      {counts && (
        <span id={summary} class="sr-only">
          {formatStat(counts) || "No line changes"}
        </span>
      )}
    </>
  );
}
