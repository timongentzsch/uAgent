import type { ComponentChildren } from "preact";
import { Box, Folder } from "lucide-preact";
import { DataText } from "./placeholder.tsx";
import type { Session } from "./types.ts";

export const folderName = (path = "") =>
  path.split("/").filter(Boolean).slice(-2).join("/") ||
  "Unavailable directory";

// The folder a session belongs to: a thread's panel folder, else its
// own working directory.
export const folderOf = (item: Session) => item.folder || item.cwd || "";

export default function FolderLabel({
  path,
  space,
  children,
}: {
  path?: string;
  // A space is µAgent's own, and goes by its name rather than its path.
  space?: boolean;
  children?: ComponentChildren;
}) {
  return (
    <span class="folder-label" title={space ? undefined : path}>
      {space ? <Box /> : <Folder />}
      <span>
        <DataText>{children || folderName(path)}</DataText>
      </span>
    </span>
  );
}
