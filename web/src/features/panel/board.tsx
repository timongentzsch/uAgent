import type { ComponentChildren } from "preact";
import type { Session } from "../../shared/types.ts";
import { Plus } from "lucide-preact";
import { Button, IconButton, Time } from "../../shared/ui.tsx";
import { statusOf } from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import SessionName from "../../shared/session-name.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { usePhone } from "../../shared/layout.ts";

const DONE_SHOWN = 8;

// Who is on the panel and what it started, beside its chat. Drawn from live
// session metadata, never stored. What waits on you is above the composer
// and in the sidebar's inbox, not repeated here.
export default function Board({
  threads,
  choose,
  menu,
  add,
}: {
  threads: Session[];
  choose: (id: string) => void;
  // A row's own actions: rename it, remove it.
  menu?: (item: Session) => ComponentChildren;
  // Starts a message that asks the moderator for another member.
  add?: () => void;
}) {
  const { online } = useHostContext();
  const managed = [...threads].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const working = (item: Session) => statusOf(item, online) === "working";
  const work = managed.filter((item) => item.kind === "thread" && !item.member);
  // Work that is done shows its latest few; the rest stay in the sidebar.
  const done = work.filter((item) => !working(item) && !item.pending);
  const groups: [string, Session[], number][] = [
    ["Members", managed.filter((item) => item.member), 0],
    [
      "Work",
      [
        ...work.filter((item) => working(item) || item.pending),
        ...done.slice(0, DONE_SHOWN),
      ],
      Math.max(0, done.length - DONE_SHOWN),
    ],
  ];
  return (
    <aside class="board" aria-label="Board">
      {groups.map(([label, items, earlier]) => (
        <section key={label}>
          <h2>
            {label} <span class="board-count">{items.length + earlier}</span>
            {label === "Members" && add && (
              <IconButton label="Add a member" disabled={!online} onClick={add}>
                <Plus />
              </IconButton>
            )}
          </h2>
          {!items.length && (
            <p class="board-note">
              {label === "Members"
                ? "Nobody yet. Ask the moderator for another voice."
                : "Nothing yet. The moderator starts threads for work you ask for."}
            </p>
          )}
          {items.map((item) => (
            <div class="session-row" key={item.id}>
              <ListRow
                class="board-row"
                onClick={() => choose(item.id)}
                title={<SessionName item={item} />}
                meta={
                  item.pending
                    ? "Needs your input"
                    : working(item)
                      ? item.member
                        ? "Typing…"
                        : item.activity || "Working"
                      : item.updated && <Time value={item.updated} />
                }
              />
              {menu?.(item)}
            </div>
          ))}
          {earlier > 0 && <p class="board-note">{earlier} earlier</p>}
        </section>
      ))}
    </aside>
  );
}

// A panel's chat keeps the conversation column; the board sits beside
// it. On a phone the header's Board button opens it from the right instead.
// Other conversations render unchanged.
export function PanelLayout({
  board,
  children,
}: {
  board: ComponentChildren;
  children: ComponentChildren;
}) {
  const phone = usePhone();
  if (!board || phone) return <>{children}</>;
  return (
    <div class="panel-layout">
      <div class="panel-chat">{children}</div>
      {board}
    </div>
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
