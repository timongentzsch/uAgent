import type { ComponentChildren } from "preact";
import type { Session } from "../../shared/types.ts";
import { Button, DataText, Time } from "../../shared/ui.tsx";

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
