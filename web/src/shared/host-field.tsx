import { useHosts } from "../state/sources.ts";
import { Select } from "./ui.tsx";

// Execution is chosen with the action, rather than as an application mode.
export default function HostField({
  value,
  change,
  localOnly = false,
}: {
  value: string;
  change: (host: string) => void;
  localOnly?: boolean;
}) {
  const hosts = useHosts();
  return (
    <label>
      Host
      <Select
        value={value}
        onChange={(event) => change(event.currentTarget.value)}
      >
        {(
          hosts?.sources || [{ id: "", name: "Local", phase: "connected" }]
        ).map((source) => (
          <option
            key={source.id}
            value={source.id}
            disabled={localOnly && !!source.id}
          >
            {source.name}
            {source.phase === "connected" ? "" : ` · ${source.phase}`}
          </option>
        ))}
      </Select>
    </label>
  );
}
