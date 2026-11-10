import { useHosts, useMultipleHosts } from "../state/sources.ts";

export function HostLabel() {
  const hosts = useHosts();
  const multiple = useMultipleHosts();
  return hosts && multiple ? (
    <p class="small muted">
      Host · {hosts.sources.find((source) => source.id === hosts.active)?.name}
    </p>
  ) : null;
}
