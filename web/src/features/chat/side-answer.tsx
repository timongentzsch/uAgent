import { X } from "lucide-preact";
import Markdown from "../../shared/markdown-view.tsx";
import { cleanText, IconButton } from "../../shared/ui.tsx";

// A /btw answer: shown above the composer, never part of the conversation.
export default function SideAnswer({
  question,
  answer,
  close,
}: {
  question: string;
  answer?: string;
  close: () => void;
}) {
  return (
    <aside class="side-answer" aria-label="Side answer">
      <header>
        <small class="muted">btw · {cleanText(question)}</small>
        <IconButton label="Dismiss side answer" onClick={close}>
          <X />
        </IconButton>
      </header>
      {answer === undefined ? (
        <p class="muted">Asking…</p>
      ) : (
        <Markdown text={answer} />
      )}
    </aside>
  );
}
