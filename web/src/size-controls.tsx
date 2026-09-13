import type { Sizes } from "./types.ts";
import { Field } from "./ui.tsx";

export function SizeControls({
  sizes = { display: 100, text: 100 },
  change,
}: {
  sizes?: Sizes;
  change?: (value: Sizes) => void;
}) {
  return (
    <>
      {(
        [
          [
            "display",
            "Interface size",
            "Scales menus, buttons and interface labels.",
            200,
          ],
          [
            "text",
            "Conversation text size",
            "Scales messages, tool output and the text you type.",
            300,
          ],
        ] as const
      ).map(([key, label, help, max]) => (
        <Field key={key} label={label} value={`${sizes[key]}%`} help={help}>
          <input
            type="range"
            aria-label={label}
            aria-valuetext={`${sizes[key]}%`}
            min="50"
            max={max}
            step="1"
            value={sizes[key]}
            disabled={!change}
            onInput={(event) =>
              change?.({ ...sizes, [key]: Number(event.currentTarget.value) })
            }
          />
        </Field>
      ))}
      <div class="dialog-actions">
        <button
          type="button"
          disabled={!change}
          onClick={() => change?.({ display: 100, text: 100 })}
        >
          Reset sizes
        </button>
      </div>
    </>
  );
}
