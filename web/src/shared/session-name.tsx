import { DataText } from "./placeholder.tsx";
import type { Session } from "./types.ts";
import { isAgent } from "../state/attention.ts";

export default function SessionName({ item }: { item: Session }) {
  return (
    <>
      {item.parent && (
        <span class="thread-mark">
          <span aria-hidden="true">↳</span>
          <span class="sr-only">
            {isAgent(item) ? "Agent: " : "Side chat: "}
          </span>
        </span>
      )}
      <DataText>{item.title || "Untitled conversation"}</DataText>
    </>
  );
}
