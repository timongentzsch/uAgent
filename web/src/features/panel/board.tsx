import type { ComponentChildren } from "preact";
import type {
  Activity,
  InspectorTarget,
  PlanStep,
  Session,
  State,
} from "../../shared/types.ts";
import { Button, Time, cleanText } from "../../shared/ui.tsx";
import { statusOf } from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import SessionName from "../../shared/session-name.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { active, withAgents } from "../../shared/activity-status.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { count, plural } from "../../shared/quantities.ts";
import { useElapsed } from "../../shared/time.ts";
import "./panel.css";

const DONE_SHOWN = 8;
const NO_ACTIVITIES: Activity[] = [];

// What the panel works on, beside its chat: its plan, each step with the
// subagents and threads working for it, and the threads no step names.
// Drawn from live state and session metadata, never stored. What waits on
// you is above the composer and in the sidebar's inbox, not repeated here.
export default function Board({
  threads,
  choose,
  menu,
  state,
  open,
}: {
  threads: Session[];
  choose: (id: string) => void;
  // A row's own actions: rename it, remove it.
  menu?: (item: Session) => ComponentChildren;
  // The panel's own state: its plan and its subagents.
  state?: State;
  // Opens a subagent in the inspector.
  open?: (target: InspectorTarget) => void;
}) {
  const { online } = useHostContext();
  const managed = [...threads].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const working = (item: Session) => statusOf(item, online) === "working";
  const steps = state?.plan?.steps || [];
  const named = new Set(steps.map((step) => step.id));
  const agents = state?.agents || [];
  const agentOf = new Map(agents.map((child) => [child.id, child]));
  const children = withAgents(
    (state?.activities || NO_ACTIVITIES).filter(
      (item) => item.kind === "agent",
    ),
    agents,
  );
  const work = managed.filter(
    (item) => item.kind === "thread" && !named.has(item.step || ""),
  );
  // Work that is done shows its latest few; the rest stay in the sidebar.
  const done = work.filter((item) => !working(item) && !item.pending);
  const items = [
    ...work.filter((item) => working(item) || item.pending),
    ...done.slice(0, DONE_SHOWN),
  ];
  const earlier = Math.max(0, done.length - DONE_SHOWN);
  const threadRow = (item: Session) => (
    <div class="session-row" key={item.id}>
      <ListRow
        onClick={() => choose(item.id)}
        title={<SessionName item={item} />}
        meta={
          item.pending
            ? "Needs your input"
            : working(item)
              ? item.activity || "Working"
              : item.updated && <Time value={item.updated} />
        }
      />
      {menu?.(item)}
    </div>
  );
  return (
    <aside class="board" aria-label="Board">
      {(state?.plan?.goals || steps.length > 0) && (
        <section>
          <h2>
            Plan{" "}
            <span class="board-count">
              {steps.filter((step) => step.status === "done").length}/
              {steps.length}
            </span>
          </h2>
          {state?.plan?.goals && (
            <p class="board-goal">{cleanText(state.plan.goals)}</p>
          )}
          <ol class="plan-steps">
            {steps.map((step) => (
              <Step
                key={step.id}
                step={step}
                agents={children.filter(
                  (item) => agentOf.get(item.agent_id || "")?.step === step.id,
                )}
                threads={managed
                  .filter((item) => item.step === step.id)
                  .map(threadRow)}
                tokensOf={(id) => agentOf.get(id)?.tokens}
                online={online}
                open={open}
              />
            ))}
          </ol>
        </section>
      )}
      <section>
        <h2>
          Work <span class="board-count">{items.length + earlier}</span>
        </h2>
        {!items.length && (
          <p class="board-note">
            Nothing yet. The moderator starts threads for work you ask for.
          </p>
        )}
        {items.map(threadRow)}
        {earlier > 0 && <p class="board-note">{earlier} earlier</p>}
      </section>
    </aside>
  );
}

// One plan step: its status and title, and, opened, its result and what
// works for it. A running step starts open.
function Step({
  step,
  agents,
  threads,
  tokensOf,
  online,
  open,
}: {
  step: PlanStep;
  agents: Activity[];
  threads: ComponentChildren[];
  tokensOf: (id: string) => number | undefined;
  online: boolean;
  open?: (target: InspectorTarget) => void;
}) {
  const running = agents.filter(active).length;
  const led =
    step.status === "running"
      ? "running"
      : step.status === "failed"
        ? "failed"
        : "idle";
  return (
    <li class="plan-step">
      <details open={step.status === "running"}>
        <summary>
          <StatusLed state={led} />
          <span class="plan-step-title">{cleanText(step.title)}</span>
          <small>
            {step.status}
            {agents.length > 0 && ` · ${plural(agents.length, "subagent")}`}
            {running > 0 && ` · ${running} running`}
          </small>
        </summary>
        {step.result && <p class="board-note">{cleanText(step.result)}</p>}
        {threads}
        {agents.map((item) => (
          <Child
            key={item.agent_id}
            item={item}
            tokens={tokensOf(item.agent_id || "")}
            online={online}
            open={open}
          />
        ))}
      </details>
    </li>
  );
}

function Child({
  item,
  tokens,
  online,
  open,
}: {
  item: Activity;
  tokens?: number;
  online: boolean;
  open?: (target: InspectorTarget) => void;
}) {
  const elapsed = useElapsed(active(item), item.started_ms);
  return (
    <ListRow
      disabled={!online || !open}
      onClick={() => open?.({ item })}
      title={cleanText(item.name || item.label || "Subagent")}
      meta={`${item.status}${elapsed ? ` · ${elapsed}` : ""}${tokens ? ` · ${count(tokens)} tokens` : ""}`}
    />
  );
}

// What a panel is, in one line under its title, with the way to
// change what it reads.
export function PanelHelp({
  editInstructions,
}: {
  editInstructions: () => void;
}) {
  return (
    <p class="panel-subtitle">
      <span>
        Plans this folder&rsquo;s work, asks subagents its questions and hands
        changes to threads (↳); never edits files itself.
      </span>
      <Button variant="quiet" onClick={editInstructions}>
        Edit instructions
      </Button>
    </p>
  );
}
