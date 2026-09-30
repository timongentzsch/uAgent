import {
  ChartColumn,
  FileText,
  Hammer,
  Plug,
  ShieldCheck,
  SlidersHorizontal,
  Smartphone,
  Sparkles,
  Wrench,
} from "lucide-preact";
import { Group, Row } from "../../shared/ui.tsx";

export const SECTIONS = [
  ["general", "General", SlidersHorizontal],
  ["agent", "Instructions", FileText],
  ["tools", "Tools", Hammer],
  ["mcp", "MCP servers", Plug],
  ["permissions", "Permissions & allowed actions", ShieldCheck],
  ["models", "Models", Sparkles],
  ["devices", "Devices", Smartphone],
  ["usage", "Usage", ChartColumn],
  ["advanced", "Advanced", Wrench],
] as const;
export type Section = (typeof SECTIONS)[number][0];
// The list reads in groups: everyday, what the agent reads and may do, which
// models, the host and its devices, escape hatches.
const NAV: [string | undefined, Section[]][] = [
  [undefined, ["general"]],
  ["Agent", ["agent", "tools", "mcp", "permissions"]],
  [undefined, ["models"]],
  ["Host", ["devices", "usage"]],
  [undefined, ["advanced"]],
];

// Settings and its loading state draw the same list, so it never changes
// while the code arrives.
export function SettingsNav({
  current,
  select,
}: {
  current?: Section;
  select: (id: Section) => void;
}) {
  return (
    <nav class="settings-nav" aria-label="Settings sections">
      {NAV.map(([title, group], index) => (
        <Group key={index} title={title}>
          {group.map((id) => {
            const [, label, Icon] = SECTIONS.find(
              ([section]) => section === id,
            )!;
            return (
              <Row
                key={id}
                label={
                  <span class="settings-nav-label">
                    <Icon />
                    {label}
                  </span>
                }
                current={id === current}
                onClick={() => select(id)}
              />
            );
          })}
        </Group>
      ))}
    </nav>
  );
}
