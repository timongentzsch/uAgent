import { ValueSelect } from "./ui.tsx";
import { zoomSteps } from "./layout.ts";

// The zoom choice in Settings → This browser.

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
      {zoomSteps.map((step) => (
        <option key={step} value={step}>
          {step}%
        </option>
      ))}
    </ValueSelect>
  );
}
