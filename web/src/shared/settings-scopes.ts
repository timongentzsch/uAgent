import { Folder, Monitor, SlidersHorizontal, Smartphone } from "lucide-preact";

// What a setting affects is where it is found: the four scopes, each one
// row, with what it reaches said once over its page.
export const SCOPES = [
  [
    "user",
    "All conversations",
    SlidersHorizontal,
    () => "Every conversation, browser and terminal on this host.",
  ],
  [
    "project",
    "This project",
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
] as const;
export type Scope = (typeof SCOPES)[number][0];
