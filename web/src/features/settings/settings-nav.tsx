import {
  Bot,
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
  ["models", "Models", Sparkles],
  ["permissions", "Permissions", ShieldCheck],
  ["agent", "Agent", Bot],
  ["mcp", "MCP servers", Plug],
  ["devices", "Devices", Smartphone],
  ["advanced", "Advanced", Wrench],
] as const;
export type Section = (typeof SECTIONS)[number][0];
// The list reads in groups: everyday, the agent, this device, escape hatches.
const NAV: Section[][] = [
  ["general"],
  ["models", "permissions", "agent", "mcp"],
  ["devices"],
  ["advanced"],
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
      {NAV.map((group, index) => (
        <Group key={index}>
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
