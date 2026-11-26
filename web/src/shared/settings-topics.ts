import {
  BookOpen,
  Cpu,
  Info,
  Monitor,
  Puzzle,
  Server,
  ShieldCheck,
} from "lucide-preact";

// Settings, topic by topic: the host's setting groups by id, and the panels
// the client draws with the words search finds them by. Navigation, search,
// ⌘K and addresses read this one table. A host group no topic names here
// still shows, at the end of the topic the host puts it in.
export type Panel =
  | "rules"
  | "mcp-servers"
  | "skills"
  | "instructions"
  | "memories"
  | "display"
  | "notifications"
  | "install"
  | "style"
  | "devices"
  | "hosts"
  | "about"
  | "file";
export type Part =
  | { group: string }
  | { panel: Panel; label: string; words: string };

const panel = (id: Panel, label: string, words = ""): Part => ({
  panel: id,
  label,
  words,
});

export const TOPICS = [
  {
    id: "models",
    label: "Models and providers",
    icon: Cpu,
    parts: [
      { group: "conversation" },
      { group: "providers" },
      { group: "other-models" },
      { group: "requests" },
    ],
  },
  {
    id: "approvals",
    label: "Approvals and safety",
    icon: ShieldCheck,
    parts: [
      { group: "approval" },
      panel("rules", "Rules", "permissions allow always ask never forget"),
      { group: "sandbox" },
      { group: "limits" },
    ],
  },
  {
    id: "tools",
    label: "Tools and extensions",
    icon: Puzzle,
    parts: [
      { group: "web-search" },
      panel("mcp-servers", "MCP servers", "mcp server log retry enable"),
      { group: "mcp" },
      { group: "skills" },
      panel("skills", "Skills in the Library", "skills library"),
      { group: "agents" },
      { group: "tools" },
    ],
  },
  {
    id: "instructions",
    label: "Instructions and memory",
    icon: BookOpen,
    parts: [
      panel("instructions", "Instructions", "agents.md system message folder"),
      { group: "instructions" },
      { group: "memory" },
      panel("memories", "Memories in the Library", "memory library"),
      { group: "attachments" },
      { group: "history" },
    ],
  },
  {
    id: "device",
    label: "This device",
    icon: Monitor,
    parts: [
      panel(
        "display",
        "Display",
        "appearance theme dark light animations motion clock timestamps zoom",
      ),
      { group: "view" },
      panel("notifications", "Notifications", "push test"),
      panel("install", "Install", "app home screen"),
      panel("style", "Custom style sheet", "css user.css look"),
    ],
  },
  {
    id: "host",
    label: "Host and devices",
    icon: Server,
    parts: [
      { group: "host" },
      panel("devices", "Paired devices", "revoke log out this device pair"),
      panel("hosts", "Remote hosts", "ssh add host reconnect disconnect"),
      { group: "notifications" },
    ],
  },
  {
    id: "about",
    label: "About and diagnostics",
    icon: Info,
    parts: [
      panel("about", "About", "version updates chrome installed"),
      { group: "updates" },
      { group: "logs" },
      panel("file", "Settings file", "settings.json reset defaults overrides"),
    ],
  },
] as const satisfies readonly {
  id: string;
  label: string;
  icon: unknown;
  parts: readonly Part[];
}[];
export type Topic = (typeof TOPICS)[number]["id"];

export const topicOf = (id?: string) => TOPICS.find((topic) => topic.id === id);

// "approvals/rules": a topic and, optionally, a row in it (a setting's key
// or a panel); anything else is Settings itself.
export function parseAddress(address = "") {
  const [id, row] = address.split(/\/(.*)/);
  const topic = topicOf(id);
  return { topic: topic?.id, row: topic ? row || undefined : undefined };
}
