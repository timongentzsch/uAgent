import "./context-parts.css";
import { ChevronRight } from "lucide-preact";
import { useMemo, useState } from "preact/hooks";
import type { ContextParts, Session } from "../../shared/types.ts";
import { count, plural } from "../../shared/quantities.ts";
import { useResource } from "../../shared/use-resource.ts";
import {
  Button,
  Cluster,
  DataText,
  LoadError,
  Placeholder,
  SectionTitle,
  usePlaceholder,
} from "../../shared/ui.tsx";
import { useApi } from "../../state/api.ts";
import { contextLeft } from "../../state/context-summary.ts";
import { useHostContext } from "../../state/host-context.ts";

type Part = keyof ContextParts["parts"];
type Tool = ContextParts["tools"][number];
// In the order the rows and the bar show them.
const LABELS: Record<Part, string> = {
  tool_results: "Tool results",
  replies: "Replies",
  messages: "Your messages",
  files: "Files and images",
  notes: "Summaries and notes",
  tools: "Tools",
  mcp: "MCP tools",
  system: "System prompt",
  instructions: "Instructions",
  memory: "Memory",
  skills: "Skills",
};
const PARTS = Object.keys(LABELS) as Part[];
// What the rows draw while the parts load.
const SAMPLE: ContextParts = {
  tokens: 1,
  bytes: 1,
  window: 0,
  compacts_at: 0,
  parts: Object.fromEntries(PARTS.map((part) => [part, 10000])) as Record<
    Part,
    number
  >,
  tools: [],
  skills: 0,
  instructions: "",
};

const percent = (value: number) => `${Number(value.toFixed(1))}%`;
const toolName = ({ name, server }: Tool) =>
  server
    ? `${server} · ${name.startsWith(`${server}_`) ? name.slice(server.length + 1) : name}`
    : name;

// The parts are read from the conversation's runtime while it is idle; the
// headline and bar stand on the totals every conversation has.
export default function ContextSection({
  session,
  used,
  limit,
  showContext,
}: {
  session?: Session;
  used?: number;
  limit?: number;
  // Set for the open conversation, which can also be compacted from here.
  showContext?: () => void;
}) {
  const { command } = useApi();
  const { online, report } = useHostContext();
  const live = !!session?.generation && !session.turn_active && online;
  const { value, error, retry } = useResource(async () => {
    if (!live) return undefined;
    const result = await command("context_parts", session);
    if (result.pending) throw new Error("The parts are not ready yet.");
    return result.result;
  }, [session?.id, session?.generation, live, used]);
  const drawing = usePlaceholder();
  const parts = live ? value : undefined;
  const loading = drawing || (live && !parts && !error);
  const shown = parts || SAMPLE;
  const tokens = parts ? parts.tokens : used;
  const capacity = parts ? parts.window : limit;
  const [open, setOpen] = useState<Record<string, boolean>>({});

  const lists = useMemo(() => {
    const sorted = [...shown.tools].sort((a, b) => b.bytes - a.bytes);
    return {
      tools: sorted.filter((t) => t.offered && !t.server && t.name !== "skill"),
      mcp: sorted.filter((t) => t.offered && t.server),
      hidden: sorted.filter((t) => !t.offered),
    };
  }, [shown]);
  // The host's total spread over the parts by their share of the bytes.
  const scale = shown.bytes ? shown.tokens / shown.bytes : 0;
  const share = (part: number) =>
    capacity ? percent((part * 100) / capacity) : "—";
  const compacts = parts && capacity ? parts.compacts_at : 0;
  const end = compacts || capacity || 0;
  const mark = capacity ? (compacts * 100) / capacity : 0;
  const usedShare = capacity && tokens ? (tokens * 100) / capacity : 0;

  const row = (
    key: string,
    label: string,
    size: number,
    {
      detail,
      tools,
      quiet,
      ratio = share(size),
    }: { detail?: string; tools?: Tool[]; quiet?: boolean; ratio?: string },
  ) => {
    const body = (
      <>
        <span class="context-swatch" data-part={key} />
        <span>
          <DataText>{label}</DataText>
          {detail && (
            <span class="muted">
              {" "}
              <DataText>{detail}</DataText>
            </span>
          )}
        </span>
        <span>
          <DataText>{count(size)}</DataText>
        </span>
        <span class="muted">
          <DataText>{ratio}</DataText>
        </span>
      </>
    );
    return (
      <li key={key} data-quiet={quiet || undefined}>
        {tools ? (
          <>
            <Button
              variant="quiet"
              class="context-row"
              aria-expanded={!!open[key]}
              disabled={!tools.length}
              onClick={() => setOpen({ ...open, [key]: !open[key] })}
            >
              {body}
              <ChevronRight class="row-chevron" />
            </Button>
            {open[key] && !!tools.length && (
              <ul class="context-tools">
                {tools.map((tool) => (
                  <li key={tool.name}>
                    <span>{toolName(tool)}</span>
                    {count(tool.bytes * scale)}
                  </li>
                ))}
              </ul>
            )}
          </>
        ) : (
          <div class="context-row">{body}</div>
        )}
      </li>
    );
  };
  const rows = (
    <ul class="context-rows">
      {PARTS.map((part) =>
        row(part, LABELS[part], shown.parts[part] * scale, {
          detail:
            part === "tools"
              ? String(lists.tools.length)
              : part === "mcp"
                ? String(lists.mcp.length)
                : part === "skills"
                  ? `${shown.skills} listed`
                  : part === "instructions"
                    ? shown.instructions
                    : undefined,
          tools:
            part === "tools"
              ? lists.tools
              : part === "mcp"
                ? lists.mcp
                : undefined,
        }),
      )}
      {(loading || !!capacity) &&
        row(
          "free",
          loading || compacts ? "Free until it compacts" : "Free",
          Math.max(0, end - shown.tokens),
          {},
        )}
      {(loading || !!compacts) &&
        row("kept", "Kept for the summary", (capacity || 0) - compacts, {
          quiet: true,
        })}
      {row(
        "hidden",
        "Not offered",
        lists.hidden.reduce((sum, tool) => sum + tool.bytes, 0) * scale,
        {
          detail: plural(lists.hidden.length, "tool"),
          tools: lists.hidden,
          quiet: true,
          ratio: "—",
        },
      )}
    </ul>
  );
  return (
    <section class="context">
      <div class="context-title">
        <SectionTitle>Context</SectionTitle>
        <small class="muted">estimated</small>
      </div>
      <p class="context-headline">
        <strong>
          <DataText>{contextLeft(tokens, capacity)}</DataText>
        </strong>
        <small class="muted">
          <DataText>
            {capacity
              ? `${count(tokens)} of ${count(capacity)} tokens`
              : `${count(tokens)} tokens`}
          </DataText>
        </small>
      </p>
      {(drawing || !!capacity) && (
        <div
          class="context-meter"
          role="img"
          aria-label={`${percent(usedShare)} of the context window is used, ${contextLeft(tokens, capacity)}${compacts ? `; it compacts at ${percent(mark)}` : ""}`}
        >
          <div class="context-track">
            {parts
              ? PARTS.map(
                  (part) =>
                    !!parts.parts[part] && (
                      <span
                        key={part}
                        class="context-segment"
                        data-part={part}
                        style={{
                          "--share": `${(parts.parts[part] * scale * 100) / parts.window}%`,
                        }}
                      />
                    ),
                )
              : !drawing && (
                  <span
                    class="context-segment"
                    style={{ "--share": `${usedShare}%` }}
                  />
                )}
          </div>
          {!!compacts && (
            <span class="context-mark" style={{ "--at": `${mark}%` }} />
          )}
          {/* Its line stays while the parts load, so they arrive in place. */}
          <small
            class="context-caption muted"
            data-side={mark > 50 ? "end" : "start"}
            style={{ "--at": `${mark}%` }}
          >
            {compacts
              ? `compacts at ${percent(mark)}`
              : parts
                ? "no automatic compaction"
                : "\u00a0"}
          </small>
        </div>
      )}
      {loading ? (
        <div>
          <Placeholder label="Loading the parts…">{rows}</Placeholder>
        </div>
      ) : parts ? (
        rows
      ) : error ? (
        <LoadError error={error} retry={retry} />
      ) : (
        <p class="muted small">
          {session?.turn_active
            ? "The parts show when this turn ends."
            : online
              ? "The parts show once this conversation runs."
              : "The parts show when the host is back online."}
        </p>
      )}
      {showContext && (
        <Cluster class="context-actions">
          <Button
            disabled={!online || !session?.generation || session.turn_active}
            onClick={() =>
              void command("submit", session, { text: "/compact" }).catch(
                report,
              )
            }
          >
            Compact now
          </Button>
          <Button disabled={!online} onClick={showContext}>
            Raw context
          </Button>
        </Cluster>
      )}
    </section>
  );
}
