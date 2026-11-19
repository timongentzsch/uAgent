import {
  LayoutDashboard,
  Palette,
  Presentation,
  ScrollText,
  StickyNote,
  Table,
} from "lucide-preact";
import { plural } from "./quantities.ts";

// Each type of artifact the host names: what a card and the Library call
// it, its icon, and what the Library opens it with.
export const ARTIFACT = {
  design: { name: "Design", icon: LayoutDashboard, open: "Open canvas" },
  deck: { name: "Deck", icon: Presentation, open: "Open deck" },
  doc: { name: "Document", icon: ScrollText, open: "Open document" },
  sheet: { name: "Sheet", icon: Table, open: "Open sheet" },
  board: { name: "Whiteboard", icon: StickyNote, open: "Open whiteboard" },
  system: { name: "Design system", icon: Palette, open: "" },
} as const;
export type ArtifactType = keyof typeof ARTIFACT;

// What one part of it is called: a deck's slide, a sheet's tab, else a page.
export const partOf = (type: ArtifactType) =>
  ({ deck: "slide", sheet: "tab" })[type as string] ?? "page";

// What it holds: a deck's slides, a sheet's tabs, a design's pages. A
// document is one.
export const artifactSize = (type: ArtifactType, count: number) =>
  type === "doc" ? "" : plural(count, partOf(type));
