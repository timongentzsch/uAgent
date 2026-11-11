import { plural } from "./quantities.ts";

// What each type of artifact is called, in a card and in the Library.
export const ARTIFACT = {
  doc: "Document",
  deck: "Deck",
  design: "Design",
  system: "Design system",
} as const;
export type ArtifactType = keyof typeof ARTIFACT;

// What it holds: a deck's slides, a design's pages. A document is one.
export const artifactSize = (type: ArtifactType, count: number) =>
  type === "doc" ? "" : plural(count, type === "deck" ? "slide" : "page");
