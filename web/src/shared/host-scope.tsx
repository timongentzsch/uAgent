import { useHosts } from "../state/sources.ts";

export function HostLabel() {
  const hosts = useHosts();
  return hosts && hosts.sources.length > 1 ? (
    <p class="small muted">
      Host · {hosts.sources.find((source) => source.id === hosts.active)?.name}
    </p>
  ) : null;
}
