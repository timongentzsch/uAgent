import { Field } from "./ui.tsx";

export const minimumZoom = 50;
export const maximumZoom = 200;
const conversationMeasure = 1040;
export const zoomChanged = "uagent:zoom";

export function normalizeZoom(value: number) {
  return Math.min(maximumZoom, Math.max(minimumZoom, value || 100));
}

export function applyZoom(value: number) {
  const zoom = normalizeZoom(value);
  document.documentElement.style.setProperty("--zoom", String(zoom / 100));
  document.documentElement.style.setProperty(
    "--conversation-measure",
    `${(conversationMeasure * 100) / zoom}px`,
  );
  dispatchEvent(new Event(zoomChanged));
}

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
          min={minimumZoom}
          max={maximumZoom}
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
