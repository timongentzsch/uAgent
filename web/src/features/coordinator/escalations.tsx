import { useEffect, useState } from "preact/hooks";
import type { Act, Pending, Report, Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import { Button, DataText, Deferred, Spinner } from "../../shared/ui.tsx";

const decisionPanel = () => import("../chat/decision.tsx");

// Decisions the folder's threads are waiting on, above the coordinator's
// composer: the one thing a user of the coordinator must never miss. Each is
// the thread's own pending decision, answered in the thread, so answering
// here or in the thread is the same act and the first answer wins.
export default function Escalations({
  sessions,
  folder,
  online,
  choose,
  report,
}: {
  sessions: Session[];
  folder: string;
  online: boolean;
  choose: (id: string) => void;
  report: Report;
}) {
  const waiting = sessions.filter(
    (item) =>
      item.pending &&
      item.kind !== "coordinator" &&
      (item.folder || item.cwd) === folder,
  );
  if (!waiting.length) return null;
  return (
    <section class="escalations" aria-label="Decisions waiting on you">
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
  const [pending, setPending] = useState<Pending | null>(null);
  useEffect(() => {
    let active = true;
    api<{ pending?: Pending | null }>(`/api/sessions/${item.id}`)
      .then((snapshot) => active && setPending(snapshot.pending || null))
      .catch(report);
    return () => {
      active = false;
    };
  }, [item.id, item.generation, item.pending, item.updated]);
  const act: Act = (kind, fields) => command(kind, item, fields);
  return (
    <div class="escalation">
      <Button
        variant="quiet"
        class="escalation-thread"
        onClick={() => choose(item.id)}
      >
        {item.kind === "thread" && <span aria-hidden="true">↳ </span>}
        <DataText>{item.title || "Untitled conversation"}</DataText>
      </Button>
      {pending ? (
        <Deferred
          load={decisionPanel}
          key={pending.id}
          pending={pending}
          act={act}
          online={online}
          report={report}
          fallback={<Spinner label="Loading decision…" surface />}
        />
      ) : (
        <Spinner label="Loading decision…" surface />
      )}
    </div>
  );
}
