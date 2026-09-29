import { useEffect, useState } from "preact/hooks";
import type { Act, Pending, Report, Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import { Button, Deferred, LoadError, Spinner } from "../../shared/ui.tsx";
import { folderOf } from "../sidebar/folder-label.tsx";
import SessionName from "../sidebar/session-name.tsx";

const decisionPanel = () => import("../chat/decision.tsx");

// Decisions the folder's threads are waiting on, above the coordinator's
// composer: the one thing a user of the coordinator must never miss. Each is
// the thread's own pending decision, answered in the thread, so answering
// here or in the thread is the same act and the first answer wins. A pause
// at the spend limit shows here too, for the same reason.
export default function Escalations({
  sessions,
  folder,
  paused,
  online,
  choose,
  report,
}: {
  sessions: Session[];
  folder: string;
  paused?: string;
  online: boolean;
  choose: (id: string) => void;
  report: Report;
}) {
  const waiting = sessions.filter(
    (item) =>
      item.pending && item.kind !== "coordinator" && folderOf(item) === folder,
  );
  if (!waiting.length && !paused) return null;
  return (
    <section class="escalations" aria-label="Decisions waiting on you">
      {paused && (
        <p role="status" class="escalation-paused">
          {paused}
        </p>
      )}
      {waiting.map((item) => (
        <Escalation
          key={item.id}
          item={item}
          online={online}
          choose={choose}
          report={report}
        />
      ))}
    </section>
  );
}

function Escalation({
  item,
  online,
  choose,
  report,
}: {
  item: Session;
  online: boolean;
  choose: (id: string) => void;
  report: Report;
}) {
  // Undefined while it loads; null once the thread no longer waits.
  const [pending, setPending] = useState<Pending | null>();
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    let active = true;
    // Never show, or answer, a decision the thread has moved past.
    setPending(undefined);
    setError(null);
    api<{ pending?: Pending | null }>(`/api/sessions/${item.id}`)
      .then((snapshot) => active && setPending(snapshot.pending || null))
      .catch((failure) => active && setError(failure));
    return () => {
      active = false;
    };
  }, [item.id, item.generation, item.updated, attempt]);
  if (pending === null) return null;
  const act: Act = (kind, fields) => command(kind, item, fields);
  const loading = <Spinner label="Loading decision…" surface />;
  return (
    <div class="escalation">
      <Button
        variant="quiet"
        class="escalation-thread"
        onClick={() => choose(item.id)}
      >
        <SessionName item={item} />
      </Button>
      {error ? (
        <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
      ) : pending ? (
        <Deferred
          load={decisionPanel}
          key={pending.id}
          pending={pending}
          session={item.id}
          act={act}
          online={online}
          report={report}
          fallback={loading}
        />
      ) : (
        loading
      )}
    </div>
  );
}
