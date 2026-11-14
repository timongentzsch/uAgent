import { useEffect, useState } from "preact/hooks";
import { localApi } from "../../state/api.ts";
import { hostLatencyPollMs } from "../../shared/limits.ts";

export default function HostLatency({ id }: { id: string }) {
  const [latency, setLatency] = useState<number | null>();
  useEffect(() => {
    const abort = new AbortController();
    let timer: ReturnType<typeof setTimeout>;
    const measure = async () => {
      try {
        const result = await localApi.api<{ round_trip_ms: number }>(
          localApi.routes.hostPing(id),
          undefined,
          { signal: abort.signal },
        );
        if (!abort.signal.aborted) setLatency(Math.round(result.round_trip_ms));
      } catch {
        if (!abort.signal.aborted) setLatency(null);
      }
      if (!abort.signal.aborted) timer = setTimeout(measure, hostLatencyPollMs);
    };
    void measure();
    return () => {
      abort.abort();
      clearTimeout(timer);
    };
  }, [id]);
  return (
    <p
      class="small muted"
      role="status"
      title="Measured from the main machine to this host."
    >
      Main → host ·{" "}
      {latency === undefined
        ? "Measuring…"
        : latency === null
          ? "Measurement unavailable"
          : `${latency} ms round trip`}
    </p>
  );
}
