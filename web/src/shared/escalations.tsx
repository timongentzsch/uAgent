import { useEffect } from "preact/hooks";
import { useResource } from "./use-resource.ts";
import type { Act, Pending, Session } from "./types.ts";
import { useApi } from "../state/api.ts";
import { useHostContext } from "../state/host-context.ts";
import { Button, LoadError, Spinner } from "./ui.tsx";
import Decision from "./decision.tsx";
import { waiting } from "../state/attention.ts";
import SessionName from "./session-name.tsx";
import "./escalations.css";

// Each is the session's own pending decision, answered in the session, so
// answering here or there is the same act and the first answer wins.
export default function WaitingList({
  sessions,
  choose,
}: {
  sessions: Session[];
  choose: (id: string) => void;
}) {
  const decisions = waiting(sessions);
  if (!decisions.length) return null;
  return (
    <section class="escalations" aria-label="Decisions waiting on you">
      {decisions.map((item) => (
        <Escalation key={item.id} item={item} choose={choose} />
      ))}
    </section>
  );
}

// The decision a session waits on: undefined while it loads, null once the
// session no longer waits.
function usePendingDecision(item: Session) {
  const { api, routes } = useApi();
  const resource = useResource(
    () =>
      api<{ pending?: Pending | null }>(routes.session(item.id)).then(
        (snapshot) => snapshot.pending || null,
      ),
    [item.id, item.generation, item.updated],
  );
  // A new session or worker never shows the last one's decision. A mere
  // update refetches in place, so an answer being typed survives it; the
  // host refuses a reply to an interaction the session has moved past.
  useEffect(() => resource.setValue(undefined), [item.id, item.generation]);
  return resource;
}

function Escalation({
  item,
  choose,
}: {
  item: Session;
  choose: (id: string) => void;
}) {
  const { command } = useApi();
  const { online, report } = useHostContext();
  const { value: pending, error, retry } = usePendingDecision(item);
  if (pending === null) return null;
  const act: Act = (kind, fields) => command(kind, item, fields);
  const loading = <Spinner label="Loading decision…" surface />;
  return (
    <div class="escalation">
      <Button
        variant="quiet"
        class="escalation-thread small"
        onClick={() => choose(item.id)}
      >
        <SessionName item={item} />
      </Button>
      {error ? (
        <LoadError error={error} retry={retry} />
      ) : pending ? (
        <Decision
          key={pending.id}
          pending={pending}
          session={item.id}
          act={act}
          online={online}
          report={report}
        />
      ) : (
        loading
      )}
    </div>
  );
}
