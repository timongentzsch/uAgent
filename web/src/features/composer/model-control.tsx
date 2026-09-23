import type { ComponentProps } from "preact";
import type ModelPicker from "../settings/model-picker.tsx";
import { Gauge, ChevronDown } from "lucide-preact";
import { Deferred } from "../../shared/ui.tsx";
import { ModelLoading } from "../../shared/loading.tsx";
import { Popover } from "../../shared/popover.tsx";

const modelPicker = () => import("../settings/model-picker.tsx");
export default function ModelControl(
  props: Omit<ComponentProps<typeof ModelPicker>, "close">,
) {
  const label = props.selection || props.state?.route || "Select model";
  return (
    <Popover
      label="Model and effort"
      title={label}
      side="top"
      align="start"
      className="model-control"
      buttonClass="quiet model-selector with-icon"
      disabled={!props.online || props.running}
      trigger={
        <>
          <Gauge />
          <span>{label}</span>
          <ChevronDown />
        </>
      }
    >
      {(close) => (
        <Deferred
          load={modelPicker}
          {...props}
          close={close}
          fallback={<ModelLoading close={close} />}
        />
      )}
    </Popover>
  );
}
