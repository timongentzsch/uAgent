import { Field } from "./ui.tsx";

export function SizeControls({
  zoom = 100,
  change,
}: {
  zoom?: number;
  change?: (value: number) => void;
}) {
  return (
    <>
      <Field
        label="Zoom"
        value={`${zoom}%`}
        help="Scales the entire interface, conversation included — like browser zoom."
      >
        <input
          type="range"
          aria-label="Zoom"
          aria-valuetext={`${zoom}%`}
          min="50"
          max="200"
          step="1"
          value={zoom}
          disabled={!change}
          onInput={(event) => change?.(Number(event.currentTarget.value))}
        />
      </Field>
      <div class="dialog-actions">
        <button type="button" disabled={!change} onClick={() => change?.(100)}>
          Reset zoom
        </button>
      </div>
    </>
  );
}
