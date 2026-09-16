// Shared expandable loader: one state machine for truncated content.
// Replaces ad-hoc full/expanding/loadError/retry in message/raw/activity.
import { useEffect, useState } from "preact/hooks";

export function useExpandable(opts: {
  online: boolean;
  expanded: boolean;
  truncated?: boolean;
  hasFull: boolean;
  load: () => Promise<string>;
}): {
  full: string | null;
  expanding: boolean;
  error: unknown;
  retry: () => void;
  reset: () => void;
} {
  const [full, setFull] = useState<string | null>(null);
  const [expanding, setExpanding] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    if (
      !opts.online ||
      !opts.expanded ||
      !opts.truncated ||
      opts.hasFull ||
      attempt < 0
    )
      return;
    if (full !== null) return;
    const abort = new AbortController();
    setExpanding(true);
    setError(null);
    opts
      .load()
      .then((text) => {
        if (!abort.signal.aborted) setFull(text);
      })
      .catch((failure) => {
        if (!abort.signal.aborted) setError(failure);
      })
      .finally(() => {
        if (!abort.signal.aborted) setExpanding(false);
      });
    return () => abort.abort();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [opts.online, opts.expanded, opts.truncated, opts.hasFull, attempt]);

  return {
    full,
    expanding,
    error,
    retry: () => {
      setFull(null);
      setAttempt((n) => n + 1);
    },
    reset: () => setFull(null),
  };
}
