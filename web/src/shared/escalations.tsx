import { useEffect } from "preact/hooks";
import { useResource } from "./use-resource.ts";
import type { Act, Pending, Session } from "./types.ts";
import { api, command, routes } from "../state/api.ts";
import { useHostContext } from "../state/host-context.ts";
import { Button, Deferred, LoadError, Spinner } from "./ui.tsx";
import { waiting } from "../state/attention.ts";
import SessionName from "./session-name.tsx";
import { folderName } from "./folder-label.tsx";

const decisionPanel = () => import("./decision.tsx");

// Decisions sessions are waiting on: a folder's threads above its
// panel's composer, or every folder's in the sidebar's inbox. Each is
// the session's own pending decision, answered in the session, so answering
// here or there is the same act and the first answer wins. A pause at the
// spend limit shows here too, for the same reason.
export default function WaitingList({
  sessions,
  folder,
  paused,
  choose,
}: {
  sessions: Session[];
  // One folder's threads; every conversation when absent.
  folder?: string;
  paused?: string;
  choose: (id: string) => void;
}) {
  const decisions = waiting(sessions, folder).filter(
    (item) => folder === undefined || item.kind !== "panel",
  );
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
          where={folder === undefined}
          choose={choose}
        />
      ))}
    </section>
  );
}

// The decision a session waits on: undefined while it loads, null once the
// session no longer waits.
function usePendingDecision(item: Session) {
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
  where,
  choose,
}: {
  item: Session;
  // Name the folder and the question: the list spans folders.
  where: boolean;
  choose: (id: string) => void;
}) {
  const { online, report } = useHostContext();
  const { value: pending, error, retry } = usePendingDecision(item);
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
        {where && (
          <span class="escalation-where">
            {" "}
            · {folderName(item.folder || item.cwd)}
            {item.pending_prompt && ` · ${item.pending_prompt}`}
          </span>
        )}
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
