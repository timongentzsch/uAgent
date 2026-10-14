import { Avatar } from "./avatar.tsx";
import { DataText } from "./placeholder.tsx";
import type { Session } from "./types.ts";

// A session's title wherever it is listed; a thread is marked ↳, and a
// member of a panel's chat has its avatar. A place's panel is its panel,
// whatever its first message was. A row that is itself a button leaves the
// member's avatar to the caller, which cannot nest one in it.
export default function SessionName({
  item,
  avatar = true,
}: {
  item: Session;
  avatar?: boolean;
}) {
  if (item.kind === "panel")
    return (
      <>
        <span class="thread-mark" aria-hidden="true">
          ◆
        </span>
        <DataText>Panel</DataText>
      </>
    );
  return (
    <>
      {item.member
        ? avatar && <Avatar name={item.member} />
        : item.kind === "thread" && (
            <span class="thread-mark">
              <span aria-hidden="true">↳</span>
              <span class="sr-only">Thread: </span>
            </span>
          )}
      <DataText>{item.title || "Untitled conversation"}</DataText>
    </>
  );
}
