import type { ComponentChildren } from "preact";
import type { Session } from "../../shared/types.ts";
import { Button, Time } from "../../shared/ui.tsx";
import { statusOf } from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import SessionName from "../../shared/session-name.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import "./panel.css";

const DONE_SHOWN = 8;

// What the panel started, beside its chat. Drawn from live
// session metadata, never stored. What waits on you is above the composer
// and in the sidebar's inbox, not repeated here.
export default function Board({
  threads,
  choose,
  menu,
}: {
  threads: Session[];
  choose: (id: string) => void;
  // A row's own actions: rename it, remove it.
  menu?: (item: Session) => ComponentChildren;
}) {
  const { online } = useHostContext();
  const managed = [...threads].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const working = (item: Session) => statusOf(item, online) === "working";
  const work = managed.filter((item) => item.kind === "thread");
  // Work that is done shows its latest few; the rest stay in the sidebar.
  const done = work.filter((item) => !working(item) && !item.pending);
  const items = [
    ...work.filter((item) => working(item) || item.pending),
    ...done.slice(0, DONE_SHOWN),
  ];
  const earlier = Math.max(0, done.length - DONE_SHOWN);
  return (
    <aside class="board" aria-label="Board">
      <section>
        <h2>
          Work <span class="board-count">{items.length + earlier}</span>
        </h2>
        {!items.length && (
          <p class="board-note">
            Nothing yet. The moderator starts threads for work you ask for.
          </p>
        )}
        {items.map((item) => (
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
        ))}
        {earlier > 0 && <p class="board-note">{earlier} earlier</p>}
      </section>
    </aside>
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
        Tracks this folder&rsquo;s conversations and hands work to threads (↳);
        never edits files itself.
      </span>
      <Button variant="quiet" onClick={editInstructions}>
        Edit instructions
      </Button>
    </p>
  );
}
