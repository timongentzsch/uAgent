import { ValueSelect } from "./ui.tsx";
import { maximumZoom, minimumZoom } from "./layout.ts";

// The interface zoom in steps, as a browser offers it: choosing one rescales
// once, where a slider rescaled the page under the finger dragging it.
const STEPS = [
  minimumZoom,
  67,
  75,
  80,
  90,
  100,
  110,
  125,
  150,
  175,
  maximumZoom,
];

export function ZoomSelect({
  zoom,
  change,
}: {
  zoom: number;
  change: (value: number) => void;
}) {
  return (
    <ValueSelect
      aria-label="Zoom"
      value={String(zoom)}
      onChange={(event) => change(Number(event.currentTarget.value))}
    >
      {STEPS.map((step) => (
        <option key={step} value={step}>
          {step}%
        </option>
      ))}
    </ValueSelect>
  );
}
