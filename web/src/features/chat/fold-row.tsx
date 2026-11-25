import type { TargetedKeyboardEvent } from "preact";
import type { ComponentChildren } from "preact";
import { ChevronRight } from "lucide-preact";
import { Button } from "../../shared/ui.tsx";

// A line that opens: a chevron, what leads it, its headline and its facts on
// the right. A transcript's steps, a merged side chat, a note or report from
// an agent and the line that unfolds earlier steps all draw it.
export function FoldRow({
  open,
  openable = true,
  onToggle,
  onKeyDown,
  bodyId,
  sequences,
  lead,
  headline,
  title,
  facts,
  factsTitle,
  failed,
}: {
  open: boolean;
  openable?: boolean;
  onToggle?: () => void;
  onKeyDown?: (event: TargetedKeyboardEvent<HTMLButtonElement>) => void;
  bodyId?: string;
  // What a closed row holds, so find can open it.
  sequences?: string;
  lead?: ComponentChildren;
  headline: ComponentChildren;
  title?: string;
  facts?: ComponentChildren;
  factsTitle?: string;
  failed?: boolean;
}) {
  const content = (
    <>
      <ChevronRight class="row-chevron" aria-hidden="true" />
      {lead}
      <span class="node-headline" title={title}>
        {failed && <span class="sr-only">Failed: </span>}
        {headline}
      </span>
      {facts && (
        <small class="node-word" title={factsTitle}>
          {facts}
        </small>
      )}
    </>
  );
  return openable ? (
    <Button
      variant="quiet"
      class="node-row"
      aria-expanded={open}
      aria-controls={open ? bodyId : undefined}
      data-sequences={sequences}
      onClick={onToggle}
      onKeyDown={onKeyDown}
    >
      {content}
    </Button>
  ) : (
    <div class="node-row">{content}</div>
  );
}
