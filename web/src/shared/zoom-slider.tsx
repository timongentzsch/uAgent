import { Input } from "./ui.tsx";
import { minimumZoom, maximumZoom } from "./layout.ts";

// The interface zoom as a slider with its value; Reset belongs to the row.
export function ZoomSlider({
  zoom = 100,
  change,
}: {
  zoom?: number;
  change?: (value: number) => void;
}) {
  return (
    <>
      <Input
        id="zoom"
        type="range"
        aria-valuetext={`${zoom}%`}
        min={minimumZoom}
        max={maximumZoom}
        step="1"
        value={zoom}
        disabled={!change}
        onInput={(event) => change?.(Number(event.currentTarget.value))}
      />
      <span class="zoom-value">{zoom}%</span>
    </>
  );
}
