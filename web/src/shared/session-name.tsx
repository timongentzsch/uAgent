import { DataText } from "./placeholder.tsx";
import type { Session } from "./types.ts";

// A session's title wherever it is listed; a thread is marked ↳. A place's
// panel is its panel, whatever its first message was.
export default function SessionName({ item }: { item: Session }) {
  if (item.kind === "panel")
    return (
      <>
        <span class="thread-mark" aria-hidden="true">
          ◆
        </span>
        <DataText>Lead</DataText>
      </>
    );
  return (
    <>
      {item.kind === "thread" && (
        <span class="thread-mark">
          <span aria-hidden="true">↳</span>
          <span class="sr-only">Thread: </span>
        </span>
      )}
      <DataText>{item.title || "Untitled conversation"}</DataText>
    </>
  );
}
