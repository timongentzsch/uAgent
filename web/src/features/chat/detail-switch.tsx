import { Button } from "../../shared/ui.tsx";
import { levelLabel } from "../../shared/verbosity.ts";

// How much of the agent's work the transcript shows: the level in effect,
// and one press to another. The host names the levels.
export function DetailSwitch({
  levels,
  level,
  disabled,
  change,
}: {
  levels: string[];
  level: string;
  disabled: boolean;
  change: (level: string) => void;
}) {
  return (
    <div class="tabs detail-switch" role="group" aria-label="Detail">
      {levels.map((item) => (
        <Button
          key={item}
          size="compact"
          disabled={disabled}
          aria-pressed={item === level}
          onClick={() => change(item)}
        >
          {levelLabel(item)}
        </Button>
      ))}
    </div>
  );
}
