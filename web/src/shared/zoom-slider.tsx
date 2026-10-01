import { Input } from "./ui.tsx";
import { minimumZoom, maximumZoom } from "./layout.ts";

// The interface zoom as a slider with its value. With `reset`, the value is
// a button that returns to the default: a slider is hard to land exactly.
export function ZoomSlider({
  zoom = 100,
  change,
  reset,
}: {
  zoom?: number;
  change?: (value: number) => void;
  reset?: () => void;
}) {
  return (
    <>
      <Input
        id="zoom"
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
      {reset ? (
        <button
          type="button"
          class="quiet zoom-value"
          aria-label={`Zoom ${zoom}%, reset`}
          title="Reset zoom"
          onClick={reset}
        >
          {zoom}%
        </button>
      ) : (
        <span class="zoom-value">{zoom}%</span>
      )}
    </>
  );
}
