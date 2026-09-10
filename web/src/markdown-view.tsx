import "./markdown.css";
import { render } from "preact";
import { Check, Copy } from "lucide-preact";
import { copyText } from "./ui.tsx";
import { failure } from "./types.ts";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
function CodeCopy({ text }: { text: string }) {
  const [status, setStatus] = useState("");
  useEffect(() => {
    if (!status) return;
    const timer = setTimeout(() => setStatus(""), 2000);
    return () => clearTimeout(timer);
  }, [status]);
  return (
    <>
      <button
        type="button"
        class="icon-button"
        title={status || "Copy code"}
        aria-label={status || "Copy code"}
        onClick={async () => {
          try {
            await copyText(text);
            setStatus("Copied!");
          } catch (error) {
            setStatus(failure(error).message);
          }
        }}
      >
        {status === "Copied!" ? <Check /> : <Copy />}
      </button>
      <span class="sr-only" role="status">
        {status}
      </span>
    </>
  );
}
let renderer: Promise<typeof import("./markdown.ts")> | undefined;
export default function Markdown({
  text,
  streaming,
}: {
  text: string;
  streaming?: boolean;
}) {
  const [html, setHTML] = useState("");
  const root = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const targets = root.current?.querySelectorAll<HTMLElement>(".code-copy");
    targets?.forEach((target) => {
      const text =
        target.parentElement?.querySelector("pre > code")?.textContent || "";
      render(<CodeCopy text={text} />, target);
    });
    return () => targets?.forEach((target) => render(null, target));
  }, [html]);
  const pending = useRef<{
    timer: ReturnType<typeof setTimeout> | undefined;
    active: boolean;
    text: string;
    streaming?: boolean;
  }>({ timer: undefined, active: true, text });
  pending.current.text = text;
  pending.current.streaming = streaming;
  useEffect(() => {
    const state = pending.current;
    const paint = async () => {
      const value = state.text;
      renderer ??= import("./markdown.ts");
      const output = await (await renderer).renderMarkdown(value);
      if (!state.active) return;
      setHTML(output);
      state.timer = undefined;
      if (value !== state.text)
        state.timer = setTimeout(paint, state.streaming ? 120 : 0);
    };
    state.timer ??= setTimeout(paint, streaming ? 120 : 0);
  }, [text, streaming]);
  useEffect(
    () => () => {
      pending.current.active = false;
      clearTimeout(pending.current.timer);
    },
    [],
  );
  return html ? (
    <div
      ref={root}
      class="markdown"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  ) : (
    <div class="plain">{text}</div>
  );
}
