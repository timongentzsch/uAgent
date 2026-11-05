import type { Ref } from "preact";
import { ArrowDown } from "lucide-preact";
import { Button } from "./ui.tsx";
import "./jump-to-latest.css";

// Shown while a transcript is not following its end; the count of rows that
// arrived meanwhile is part of the name, so it is announced too.
export function JumpToLatest({
  unseen,
  onClick,
  buttonRef,
}: {
  unseen: number;
  onClick: () => void;
  buttonRef?: Ref<HTMLButtonElement>;
}) {
  return (
    <Button
      variant="quiet"
      class="jump enters"
      buttonRef={buttonRef}
      onClick={onClick}
    >
      Jump to latest{unseen > 0 && <span> ({unseen} new)</span>}
      <ArrowDown />
    </Button>
  );
}
