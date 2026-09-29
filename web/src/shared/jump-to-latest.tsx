import { ArrowDown } from "lucide-preact";
import { Button } from "./ui.tsx";

// Shown while a transcript is not following its end; the count of rows that
// arrived meanwhile is part of the name, so it is announced too.
export function JumpToLatest({
  unseen,
  onClick,
}: {
  unseen: number;
  onClick: () => void;
}) {
  return (
    <Button variant="quiet" class="jump" onClick={onClick}>
      Jump to latest
      {unseen > 0 && <span>({unseen} new)</span>}
      <ArrowDown />
    </Button>
  );
}
