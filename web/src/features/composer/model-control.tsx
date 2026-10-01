import type { ComponentProps } from "preact";
import type ModelPicker from "./model-picker.tsx";
import { Gauge, ChevronDown } from "lucide-preact";
import { Deferred, DataText } from "../../shared/ui.tsx";
import { ModelLoading } from "../../shared/loading.tsx";
import { SheetButton } from "../../shared/sheet.tsx";

const modelPicker = () => import("./model-picker.tsx");
export default function ModelControl(
  props: Omit<ComponentProps<typeof ModelPicker>, "close">,
) {
  const label = props.selection || props.state?.route || "Select model";
  return (
    <SheetButton
      label="Model and effort"
      title={label}
      className="model-control"
      buttonClass="quiet model-selector"
      disabled={!props.online || props.running}
      trigger={
        <>
          <Gauge />
          <span>
            <DataText>{label}</DataText>
          </span>
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
    </SheetButton>
  );
}
