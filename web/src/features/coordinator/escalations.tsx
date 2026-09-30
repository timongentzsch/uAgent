import { useEffect } from "preact/hooks";
import { useResource } from "../../shared/use-resource.ts";
import type { Act, Pending, Report, Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import { Button, Deferred, LoadError, Spinner } from "../../shared/ui.tsx";
import { waiting } from "../../state/attention.ts";
import SessionName from "../../shared/session-name.tsx";

const decisionPanel = () => import("../chat/decision.tsx");

// Decisions the folder's threads are waiting on, above the coordinator's
// composer: the one thing a user of the coordinator must never miss. Each is
// the thread's own pending decision, answered in the thread, so answering
// here or in the thread is the same act and the first answer wins. A pause
// at the spend limit shows here too, for the same reason.
export default function Escalations({
  threads,
  paused,
  online,
  choose,
  report,
}: {
  threads: Session[];
  paused?: string;
  online: boolean;
  choose: (id: string) => void;
  report: Report;
}) {
  const decisions = waiting(threads);
  if (!decisions.length && !paused) return null;
  return (
    <section class="escalations" aria-label="Decisions waiting on you">
      {paused && (
        <p role="status" class="escalation-paused">
          {paused}
        </p>
      )}
      {decisions.map((item) => (
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
  const {
    value: pending,
    error,
    retry,
    setValue: setPending,
  } = useResource(
    () =>
      api<{ pending?: Pending | null }>(`/api/sessions/${item.id}`).then(
        (snapshot) => snapshot.pending || null,
      ),
    [item.id, item.generation, item.updated],
  );
  // A new thread or worker never shows the last one's decision. A mere
  // update refetches in place, so an answer being typed survives it; the
  // host refuses a reply to an interaction the thread has moved past.
  useEffect(() => setPending(undefined), [item.id, item.generation]);
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
        <LoadError error={error} retry={retry} />
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
