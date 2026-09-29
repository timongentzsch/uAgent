import type { ComponentChildren } from "preact";
import { CircleHelp } from "lucide-preact";
import { Popover } from "../../shared/popover.tsx";
import type { Session } from "../../shared/types.ts";
import { Actions, Button, DataText, Time } from "../../shared/ui.tsx";

// The folder at a glance, beside its coordinator: what waits on you, what is
// working, and what is done. Drawn from live session metadata, never stored.
export default function Board({
  sessions,
  folder,
  online,
  choose,
}: {
  sessions: Session[];
  folder: string;
  online: boolean;
  choose: (id: string) => void;
}) {
  const managed = sessions
    .filter(
      (item) =>
        item.kind !== "coordinator" && (item.folder || item.cwd) === folder,
    )
    .sort((a, b) => (b.updated || 0) - (a.updated || 0));
  const groups: [string, Session[]][] = [
    ["Needs you", managed.filter((item) => item.pending)],
    [
      "Working",
      managed.filter((item) => !item.pending && online && item.turn_active),
    ],
    [
      "Done",
      managed.filter((item) => !item.pending && !(online && item.turn_active)),
    ],
  ];
  // Only groups with something in them; an empty folder says so once.
  const shown = groups.filter(([, items]) => items.length);
  return (
    <aside class="board" aria-label="Board">
      {!shown.length && <p class="board-empty">No sessions in this folder</p>}
      {shown.map(([label, items]) => (
        <section key={label}>
          <h2>
            {label} <span class="board-count">{items.length}</span>
          </h2>
          {items.map((item) => (
            <Button
              key={item.id}
              variant="quiet"
              class="board-row"
              onClick={() => choose(item.id)}
            >
              <span>
                {item.kind === "thread" && (
                  <span class="thread-mark" aria-label="Thread">
                    ↳
                  </span>
                )}
                <DataText>{item.title || "Untitled conversation"}</DataText>
              </span>
              <small>
                {item.pending
                  ? "Waiting for your decision"
                  : online && item.turn_active
                    ? item.activity || "Working"
                    : item.updated && <Time value={item.updated} />}
              </small>
            </Button>
          ))}
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
export function CoordinatorHelp({ editSoul }: { editSoul: () => void }) {
  return (
    <Popover
      label="What is the coordinator?"
      trigger={<CircleHelp aria-hidden="true" />}
      className="coordinator-help-anchor"
      panelClass="coordinator-help"
      align="start"
    >
      {(close) => (
        <div class="coordinator-help-body">
          <h2>The folder's coordinator</h2>
          <p>
            One per folder. It keeps track of every conversation here, answers
            questions about them, and hands work to threads. It reads files but
            never edits them or runs commands itself.
          </p>
          <h2>How it differs from a conversation</h2>
          <p>
            A conversation does the work you ask for in it. The coordinator
            manages conversations: it starts threads (ordinary conversations,
            marked ↳), steers them, and decides the approvals their Auto mode
            cannot settle, asking you when it is unsure. Its notes and goals
            carry over between days. You can open and steer any thread directly.
          </p>
          <h2>Its soul</h2>
          <p>
            Standing guidance it reads every turn, yours and a trusted
            project's. It is edited with the system prompt.
          </p>
          <Actions>
            <Button
              variant="secondary"
              onClick={() => {
                close();
                editSoul();
              }}
            >
              Edit soul
            </Button>
          </Actions>
        </div>
      )}
    </Popover>
  );
}
