import type { ComponentChildren } from "preact";
import { Folder } from "lucide-preact";
import { DataText } from "../../shared/placeholder.tsx";
import type { Session } from "../../shared/types.ts";

export const folderName = (path = "") =>
  path.split("/").filter(Boolean).slice(-2).join("/") ||
  "Unavailable directory";

// The folder a session belongs to: a thread's coordinator folder, else its
// own working directory.
export const folderOf = (item: Session) => item.folder || item.cwd || "";

export default function FolderLabel({
  path,
  children,
}: {
  path?: string;
  children?: ComponentChildren;
}) {
  return (
    <span class="folder-label" title={path}>
      <Folder aria-hidden="true" />
      <span>
        <DataText>{children || folderName(path)}</DataText>
      </span>
    </span>
  );
}
