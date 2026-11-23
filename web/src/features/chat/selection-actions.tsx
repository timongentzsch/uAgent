import { useEffect, useState } from "preact/hooks";
import { highlights } from "../../shared/find.ts";
import { maxQuote } from "../../shared/limits.ts";
import { Button } from "../../shared/ui.tsx";

// The mark on a passage while its question is written: the selection itself
// goes to the field.
const asked = "asked";

// Text as the quote a message opens with.
const quoted = (text: string) =>
  (text.length > maxQuote ? `${text.slice(0, maxQuote)} […]` : text)
    .split("\n")
    .map((line) => `> ${line}`)
    .join("\n");

// `x`, `y`: the end of what is selected; `left`: where it starts; `right`:
// where its transcript ends.
type Picked = {
  text: string;
  side?: string;
  x: number;
  y: number;
  left: number;
  right: number;
};

// What is selected inside one transcript: the conversation's or a side
// chat's, which is then named.
function read(): Picked | undefined {
  const selection = getSelection();
  if (!selection?.rangeCount || selection.isCollapsed) return undefined;
  const range = selection.getRangeAt(0);
  const node = range.commonAncestorContainer;
  const within = (
    node instanceof Element ? node : node.parentElement
  )?.closest<HTMLElement>(".transcript, .side-transcript");
  const text = selection.toString().trim();
  // A selection that ends at the start of the next block reports an empty
  // box there, which is not where its text ends.
  const boxes = [...range.getClientRects()].filter((box) => box.width);
  const end = boxes.at(-1);
  if (!within || !text || !end) return undefined;
  return {
    text,
    side: within.dataset.side,
    x: end.right,
    y: end.bottom,
    left: Math.min(...boxes.map((box) => box.left)),
    right: within.getBoundingClientRect().right,
  };
}

// Offered at the end of a selection; a narrow column shows them as a bar
// above the composer, clear of the system's own selection menu.
export function SelectionActions({
  asking,
  ask,
  add,
}: {
  asking: boolean;
  ask: (quote: string, side?: string) => void;
  add: (text: string) => void;
}) {
  const [picked, setPicked] = useState<Picked>();
  useEffect(() => {
    const update = () => setPicked(read());
    document.addEventListener("selectionchange", update);
    // They follow the selection as its transcript scrolls.
    addEventListener("scroll", update, { capture: true, passive: true });
    addEventListener("resize", update);
    return () => {
      document.removeEventListener("selectionchange", update);
      removeEventListener("scroll", update, { capture: true });
      removeEventListener("resize", update);
    };
  }, []);
  useEffect(() => {
    if (!asking) highlights?.delete(asked);
  }, [asking]);
  if (!picked) return null;
  const use = (take: (quote: string) => void, mark = false) => {
    const selection = getSelection()!;
    if (mark) highlights?.set(asked, new Highlight(selection.getRangeAt(0)));
    selection.removeAllRanges();
    setPicked(undefined);
    take(quoted(picked.text));
  };
  return (
    // raw: floats over the page, not along an edge of a pane.
    <div
      class="float-toolbar selection-quote"
      role="group"
      aria-label="Selection"
      style={{
        "--at-x": `${picked.x}px`,
        "--at-y": `${picked.y}px`,
        "--at-left": `${picked.left}px`,
        "--at-right": `${picked.right}px`,
      }}
      // Pressing a button must not drop the selection it quotes.
      onMouseDown={(event) => event.preventDefault()}
    >
      <p class="muted truncate">{picked.text}</p>
      <Button
        variant="primary"
        onClick={() => use((quote) => ask(quote, picked.side), true)}
      >
        Ask in side chat
      </Button>
      <Button onClick={() => use(add)}>Quote</Button>
    </div>
  );
}
