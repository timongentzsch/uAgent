import { plural } from "./quantities.ts";

// What each type of artifact is called, in a card and in the Library.
export const ARTIFACT = {
  doc: "Document",
  deck: "Deck",
  design: "Design",
  system: "Design system",
  sheet: "Sheet",
} as const;
export type ArtifactType = keyof typeof ARTIFACT;

// What one part of it is called: a deck's slide, a sheet's tab, else a page.
export const partOf = (type: ArtifactType) =>
  ({ deck: "slide", sheet: "tab" })[type as string] ?? "page";

// What it holds: a deck's slides, a sheet's tabs, a design's pages. A
// document is one.
export const artifactSize = (type: ArtifactType, count: number) =>
  type === "doc" ? "" : plural(count, partOf(type));
