import {
  Folder,
  Monitor,
  Plug,
  ShieldCheck,
  SlidersHorizontal,
  Smartphone,
  Sparkles,
  Wrench,
} from "lucide-preact";
import { Group, Row } from "../../shared/ui.tsx";

// Each section belongs to one scope: what its settings affect.
export const SECTIONS = [
  ["general", "General", SlidersHorizontal, "user"],
  ["models", "Models", Sparkles, "user"],
  ["permissions", "Permissions", ShieldCheck, "user"],
  ["mcp", "MCP servers", Plug, "user"],
  ["advanced", "Advanced", Wrench, "user"],
  ["project", "This project", Folder, "project"],
  ["display", "Display", Monitor, "browser"],
  ["devices", "Devices", Smartphone, "host"],
] as const;
export type Section = (typeof SECTIONS)[number][0];
export type Scope = (typeof SECTIONS)[number][3];
// A scope as the list names it, and what it affects and where it is saved,
// said once over every section of it.
export const SCOPES: Record<Scope, [string, (folder?: string) => string]> = {
  user: [
    "All conversations",
    () =>
      "Every conversation, browser and terminal on this host.",
  ],
  project: [
    "This project",
    (folder) =>
      `Overrides All conversations for conversations in ${folder}.`,
  ],
  browser: [
    "This browser",
    () =>
      "Saved in this browser only. Other browsers and the terminal are not affected.",
  ],
  host: ["Host", () => "This host and the browsers paired with it."],
};

// Settings and its loading state draw the same list, so it never changes
// while the code arrives. A project is listed while a conversation is open.
export function SettingsNav({
  current,
  select,
  project,
}: {
  current?: Section;
  select: (id: Section) => void;
  // The open conversation's folder, by its last name.
  project?: string;
}) {
  return (
    <nav class="settings-nav" aria-label="Settings sections">
      {(Object.keys(SCOPES) as Scope[]).map(
        (scope) =>
          (scope !== "project" || project) && (
            <Group key={scope} title={SCOPES[scope][0]}>
              {SECTIONS.filter((section) => section[3] === scope).map(
                ([id, label, Icon]) => (
                  <Row
                    key={id}
                    label={
                      <span class="settings-nav-label">
                        <Icon />
                        {id === "project" ? project : label}
                      </span>
                    }
                    current={id === current}
                    onClick={() => select(id)}
                  />
                ),
              )}
            </Group>
          ),
      )}
    </nav>
  );
}
