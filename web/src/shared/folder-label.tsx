import type { ComponentChildren } from "preact";
import { createLucideIcon, Folder } from "lucide-preact";
import { DataText } from "./placeholder.tsx";

// A workspace: four boxes of different sizes, drawn as the other icons are.
const Workspace = createLucideIcon("workspace", [
  ["rect", { x: "3", y: "4", width: "6", height: "6", rx: "1.5" }],
  ["rect", { x: "11", y: "4", width: "10", height: "6", rx: "1.5" }],
  ["rect", { x: "3", y: "12", width: "10", height: "8", rx: "1.5" }],
  ["rect", { x: "15", y: "12", width: "6", height: "6", rx: "1.5" }],
]);

export const folderName = (path = "") =>
  path.split("/").filter(Boolean).slice(-2).join("/") ||
  "Unavailable directory";

// A folder goes by its own name, with the directory it is in after it,
// dimmed; a name given in `children` stands alone. The own name keeps its
// room and the directory truncates first.
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
  const parent = children ? "" : path?.split("/").filter(Boolean).at(-2);
  return (
    <span class="folder-label" title={space ? undefined : path}>
      {space ? <Workspace /> : <Folder />}
      <span>
        <span class="folder-own">
          <DataText>
            {children ||
              path?.split("/").filter(Boolean).at(-1) ||
              folderName()}
          </DataText>
        </span>
        {parent && <small class="folder-parent">{parent}</small>}
      </span>
    </span>
  );
}
