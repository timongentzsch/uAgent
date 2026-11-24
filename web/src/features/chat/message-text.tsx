import type { Asset } from "../../shared/types.ts";
import { ImageTile } from "../../shared/attachments.tsx";
import { splitMentionTokens } from "../../shared/message-view.ts";
import Markdown from "../../shared/markdown-view.tsx";

// Resolves against the message's own files: a renamed file shows its
// current name, a removed one degrades to muted text.
function MentionFile({
  id,
  alt,
  files,
  assets,
}: {
  id: string;
  alt: string;
  files?: Asset[];
  assets: string;
}) {
  const file = files?.find((item) => item.id === id);
  if (!file) return <span class="muted">@{alt} (attachment removed)</span>;
  const href = `${assets}${file.id}`;
  return file.image ? (
    <span class="mention-image">
      <ImageTile src={href} name={file.name} />
    </span>
  ) : (
    <a class="chip file-chip mention-chip" href={href} download={file.name}>
      @{file.name}
    </a>
  );
}

export function MessageText({
  text,
  streaming,
  refs,
  files,
  assets,
}: {
  text: string;
  streaming?: boolean;
  refs: boolean;
  files?: Asset[];
  assets: string;
}) {
  return splitMentionTokens(text).map((part, index) =>
    "text" in part ? (
      part.text ? (
        // Segments render independently: emphasis spanning a token
        // keeps its words but loses the styling (rare, accepted).
        <Markdown
          key={`t-${index}`}
          text={part.text}
          streaming={streaming}
          refs={refs}
        />
      ) : null
    ) : (
      <MentionFile
        key={`m-${part.mention.id}-${index}`}
        id={part.mention.id}
        alt={part.mention.alt}
        files={files}
        assets={assets}
      />
    ),
  );
}
