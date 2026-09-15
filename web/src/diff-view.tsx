import { diffLineClass } from "./display.ts";

export default function DiffView({ text }: { text: string }) {
  const lines = text.replace(/\n$/, "").split("\n");
  return (
    <pre class="diff" aria-label="File changes" tabIndex={0}>
      {lines.map((line, index) => (
        <span key={index} class={diffLineClass(line, index === 0)}>
          {line === "" ? " " : line}
        </span>
      ))}
    </pre>
  );
}
