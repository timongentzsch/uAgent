import type { ComponentProps } from "preact";
import ModelPicker from "./model-picker.tsx";
import { Gauge, ChevronDown } from "lucide-preact";
import { DataText, updateChecked } from "./ui.tsx";
import { SheetButton } from "./sheet.tsx";
import "./model-control.css";

function ModelControl(
  props: Omit<ComponentProps<typeof ModelPicker>, "close">,
) {
  const label = props.selection || props.state?.route || "Select model";
  return (
    <SheetButton
      label="Model and effort"
      title={label}
      className="model-control"
      buttonClass="model-selector"
      disabled={!props.online || props.running}
      // A conversation with no model yet starts at the picker.
      openWhen={props.state?.route === ""}
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
      {(close) => <ModelPicker {...props} close={close} />}
    </SheetButton>
  );
}

// The picker reads the route and efforts, nothing else of the state.
export default updateChecked(ModelControl, ["route", "effort", "efforts"]);
