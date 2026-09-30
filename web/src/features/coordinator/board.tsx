import type { ComponentChildren } from "preact";
import { CircleHelp } from "lucide-preact";
import { SheetButton } from "../../shared/sheet.tsx";
import type { Session } from "../../shared/types.ts";
import { Actions, Button, Time } from "../../shared/ui.tsx";
import { waiting } from "../../state/attention.ts";
import SessionName from "../../shared/session-name.tsx";
import { ListRow } from "../../shared/list-row.tsx";

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
  // few, and the rest stay in the sidebar.
  const groups: [string, Session[], number][] = [
    ["Needs you", waiting(managed), Infinity],
    [
      "Working",
      managed.filter((item) => !item.pending && working(item)),
      Infinity,
    ],
    [
      "Done",
      managed.filter((item) => !item.pending && !working(item)),
      DONE_SHOWN,
    ],
  ];
  // Only groups with something in them; an empty folder says so once.
  const shown = groups.filter(([, items]) => items.length);
  return (
    <aside class="board" aria-label="Board">
      {!shown.length && <p class="board-note">No sessions in this folder</p>}
      {shown.map(([label, items, limit]) => (
        <section key={label}>
          <h2>
            {label} <span class="board-count">{items.length}</span>
          </h2>
          {items.slice(0, limit).map((item) => (
            <ListRow
              key={item.id}
              variant="quiet"
              class="board-row"
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
// it, or above it on a phone. Other conversations render unchanged.
export function CoordinatorLayout({
  board,
  children,
}: {
  board: ComponentChildren;
  children: ComponentChildren;
}) {
  if (!board) return <>{children}</>;
  return (
    <div class="coordinator-layout">
      <div class="coordinator-chat">{children}</div>
      {board}
    </div>
  );
}

// What a coordinator is, next to its title: a tap opens it, so it works
// without hover on a phone.
export function CoordinatorHelp({
  editInstructions,
}: {
  editInstructions: () => void;
}) {
  return (
    <SheetButton
      label="What is the coordinator?"
      trigger={<CircleHelp />}
      className="coordinator-help-anchor"
      heading="The folder's coordinator"
      sheetClass="coordinator-help"
    >
      {(close) => (
        <div class="coordinator-help-body">
          <p>
            One per folder. It keeps track of every conversation here, answers
            questions about them, and hands work to threads. It reads files but
            never edits them or runs commands itself.
          </p>
          <h3>How it differs from a conversation</h3>
          <p>
            A conversation does the work you ask for in it. The coordinator
            manages conversations: it starts threads (ordinary conversations,
            marked ↳), steers them, and decides the approvals their Auto mode
            cannot settle, asking you when it is unsure. Its notes and goals
            carry over between days. You can open and steer any thread directly.
          </p>
          <h3>Its instructions</h3>
          <p>
            It reads every session's AGENTS.md, then its own COORDINATOR.md:
            yours, and the folder's.
          </p>
          <Actions>
            <Button
              variant="secondary"
              onClick={() => {
                close();
                editInstructions();
              }}
            >
              Edit instructions
            </Button>
          </Actions>
        </div>
      )}
    </SheetButton>
  );
}
