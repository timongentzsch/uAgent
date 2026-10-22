import { ValueSelect } from "./ui.tsx";
import { maximumZoom, minimumZoom } from "./layout.ts";

// The interface zoom in steps, as a browser offers it: choosing one rescales
// once, where a slider rescaled the page under the finger dragging it. A
// zoom kept from before that is no step stays listed.
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
  const steps = STEPS.includes(zoom)
    ? STEPS
    : [...STEPS, zoom].sort((a, b) => a - b);
  return (
    <ValueSelect
      aria-label="Zoom"
      value={String(zoom)}
      onChange={(event) => change(Number(event.currentTarget.value))}
    >
      {steps.map((step) => (
        <option key={step} value={step}>
          {step}%
        </option>
      ))}
    </ValueSelect>
  );
}
