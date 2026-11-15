import {
  Folder,
  Info,
  Monitor,
  SlidersHorizontal,
  Smartphone,
} from "lucide-preact";

export const SCOPES = [
  [
    "user",
    "All conversations",
    SlidersHorizontal,
    () => "Every conversation, browser and terminal on this host.",
  ],
  [
    "project",
    "This folder",
    Folder,
    (folder?: string) =>
      `Overrides All conversations for conversations in ${folder}.`,
  ],
  [
    "browser",
    "This browser",
    Monitor,
    () =>
      "Saved in this browser only. Other browsers and the terminal are not affected.",
  ],
  [
    "host",
    "Host",
    Smartphone,
    () => "This host and the browsers paired with it.",
  ],
  ["about", "About", Info, () => "This µAgent and its updates."],
] as const;
export type Scope = (typeof SCOPES)[number][0];
