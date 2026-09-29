import { DataText } from "./placeholder.tsx";
import type { Session } from "./types.ts";

// A session's title wherever it is listed; a thread is marked ↳.
export default function SessionName({ item }: { item: Session }) {
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
