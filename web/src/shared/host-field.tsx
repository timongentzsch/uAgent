import { useHosts } from "../state/sources.ts";
import { connectionLabels } from "./connection-status.tsx";
import { Field, Select } from "./ui.tsx";

// With no remote host there is nothing to choose.
export default function HostField({
  label = "Host",
  value,
  change,
  localOnly = false,
}: {
  label?: string;
  value: string;
  change: (host: string) => void;
  localOnly?: boolean;
}) {
  const { sources } = useHosts();
  if (sources.length < 2) return null;
  return (
    <Field label={label}>
      <Select
        value={value}
        onChange={(event) => change(event.currentTarget.value)}
      >
        {sources.map((source) => (
          <option
            key={source.id}
            value={source.id}
            disabled={localOnly && !!source.id}
          >
            {source.name}
            {source.phase === "connected"
              ? ""
              : ` · ${connectionLabels[source.phase]}`}
          </option>
        ))}
      </Select>
    </Field>
  );
}
