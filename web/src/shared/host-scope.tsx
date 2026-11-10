import { useHosts } from "../state/sources.ts";
import { Select } from "./ui.tsx";

// Global pages operate on one host. Device and pairing pages name their
// serving host explicitly instead of inheriting the selected remote.
export function HostScope({
  device = false,
  settings,
}: {
  device?: boolean;
  settings?: string;
}) {
  const hosts = useHosts();
  if (!hosts || hosts.sources.length < 2) return null;
  if (device)
    return (
      <p class="small muted">
        Device and pairing settings · Local serving host
      </p>
    );
  return (
    <label class="small">
      Host
      <Select
        aria-label="Host"
        value={hosts.active}
        onChange={(event) =>
          hosts.open(event.currentTarget.value, "", "", { settings })
        }
      >
        {hosts.sources.map((source) => (
          <option key={source.id} value={source.id}>
            {source.name}
          </option>
        ))}
      </Select>
    </label>
  );
}

export function HostLabel() {
  const hosts = useHosts();
  return hosts && hosts.sources.length > 1 ? (
    <p class="small muted">
      Host · {hosts.sources.find((source) => source.id === hosts.active)?.name}
    </p>
  ) : null;
}
