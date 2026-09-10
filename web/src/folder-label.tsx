import type { ComponentChildren } from "preact";
import { Folder } from "lucide-preact";

export const folderName = (path = "") =>
  path.split("/").filter(Boolean).slice(-2).join("/") ||
  "Unavailable directory";

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
      <span>{children || folderName(path)}</span>
    </span>
  );
}
