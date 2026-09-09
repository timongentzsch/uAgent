import { useEffect, useRef, useState } from "preact/hooks";
let renderer: Promise<typeof import("./markdown.ts")> | undefined;
export default function Markdown({
  text,
  streaming,
}: {
  text: string;
  streaming?: boolean;
}) {
  const [html, setHTML] = useState("");
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
    <div class="markdown" dangerouslySetInnerHTML={{ __html: html }} />
  ) : (
    <div class="plain">{text}</div>
  );
}
