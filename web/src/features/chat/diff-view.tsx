import { useId } from "preact/hooks";
import { diffLineClass } from "../../shared/display.ts";
import { diffCounts, formatStat } from "./tool-preview.ts";

// A scrollable region, so a keyboard can scroll it; a listener hears the
// size of the change before its lines.
export default function DiffView({ text }: { text: string }) {
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
        {formatStat(diffCounts(text)) || "No line changes"}
      </span>
    </>
  );
}
