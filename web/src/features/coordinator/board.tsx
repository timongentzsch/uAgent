import type { ComponentChildren } from "preact";
import type { Session } from "../../shared/types.ts";
import { Button, Time } from "../../shared/ui.tsx";
import { waiting } from "../../state/attention.ts";
import SessionName from "../../shared/session-name.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { usePhone } from "../../shared/layout.ts";

const DONE_SHOWN = 8;

// The folder at a glance, beside its coordinator: what waits on you, what is
// working, and what is done. Drawn from live session metadata, never stored.
export default function Board({
  threads,
  online,
  choose,
}: {
  threads: Session[];
  online: boolean;
  choose: (id: string) => void;
}) {
  const managed = [...threads].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const working = (item: Session) => online && item.turn_active;
  // Each group with how many rows it lists: what is done shows the latest
  // few, and the rest stay in the sidebar. The chat's members are neither
  // working nor done: they are there.
  const groups: [string, Session[], number][] = [
    ["Needs you", waiting(managed), Infinity],
    [
      "Members",
      managed.filter((item) => !item.pending && item.member),
      Infinity,
    ],
    [
      "Working",
      managed.filter((item) => !item.pending && !item.member && working(item)),
      Infinity,
    ],
    [
      "Done",
      managed.filter((item) => !item.pending && !item.member && !working(item)),
      DONE_SHOWN,
    ],
  ];
  // Only groups with something in them; an empty folder says so once.
  const shown = groups.filter(([, items]) => items.length);
  return (
    <aside class="board" aria-label="Board">
      {!shown.length && (
        <p class="board-note">
          Nothing here yet. Threads the coordinator starts, members of its chat
          and what needs you appear here.
        </p>
      )}
      {shown.map(([label, items, limit]) => (
        <section key={label}>
          <h2>
            {label} <span class="board-count">{items.length}</span>
          </h2>
          {items.slice(0, limit).map((item) => (
            <ListRow
              key={item.id}
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
          ))}
          {items.length > limit && (
            <p class="board-note">{items.length - limit} earlier</p>
          )}
        </section>
      ))}
    </aside>
  );
}

// A coordinator's chat keeps the conversation column; the board sits beside
// it. On a phone the header's Board button opens it from the right instead.
// Other conversations render unchanged.
export function CoordinatorLayout({
  board,
  children,
}: {
  board: ComponentChildren;
  children: ComponentChildren;
}) {
  const phone = usePhone();
  if (!board || phone) return <>{children}</>;
  return (
    <div class="coordinator-layout">
      <div class="coordinator-chat">{children}</div>
      {board}
    </div>
  );
}

// What a coordinator is, in one line under its title, with the way to
// change what it reads.
export function CoordinatorHelp({
  editInstructions,
}: {
  editInstructions: () => void;
}) {
  return (
    <p class="coordinator-subtitle">
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
