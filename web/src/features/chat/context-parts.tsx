import "./context-parts.css";
import { ChevronRight } from "lucide-preact";
import { useMemo, useState } from "preact/hooks";
import {
  failure,
  type ContextParts,
  type Session,
  type State,
  type ToolCatalogueItem,
} from "../../shared/types.ts";
import { count, plural } from "../../shared/quantities.ts";
import { useResource } from "../../shared/use-resource.ts";
import {
  Button,
  Cluster,
  DataText,
  SectionTitle,
  usePlaceholder,
} from "../../shared/ui.tsx";
import { useApi } from "../../state/api.ts";
import { contextLeft } from "../../state/context-summary.ts";
import { useHostContext } from "../../state/host-context.ts";

type Part = keyof ContextParts["parts"];
type Kind = keyof ContextParts["tools"];
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
};
const PARTS = Object.keys(LABELS) as Part[];
// What a loading sheet draws.
const SAMPLE: ContextParts = {
  tokens: 1,
  bytes: 1,
  compacts_at: 0,
  instructions: "",
  parts: Object.fromEntries(PARTS.map((part) => [part, 10000])) as Record<
    Part,
    number
  >,
  tools: Object.fromEntries(
    ["tools", "mcp", "hidden"].map((kind) => [kind, { count: 1, bytes: 1 }]),
  ),
};

const percent = (value: number) => `${Number(value.toFixed(1))}%`;
const kindOf = (tool: ToolCatalogueItem): Kind =>
  !tool.active ? "hidden" : tool.category === "mcp" ? "mcp" : "tools";
const toolName = ({ name, provider, category }: ToolCatalogueItem) => {
  if (category !== "mcp") return name;
  const server = provider.replace(/^mcp:/, "");
  return `${server} · ${name.startsWith(`${server}_`) ? name.slice(server.length + 1) : name}`;
};

// The parts come with the conversation's state; a fold's tools are read
// from its runtime when the fold opens.
export default function ContextSection({
  session,
  state,
  showContext,
}: {
  session?: Session;
  state?: Pick<State, "context_tokens" | "context_window" | "context_parts">;
  // Set for the open conversation, which can also be compacted from here.
  showContext?: () => void;
}) {
  const { command } = useApi();
  const { online, report } = useHostContext();
  const drawing = usePlaceholder();
  const shown = drawing ? SAMPLE : state?.context_parts;
  // A live total ahead of the parts is the reply streaming in since its
  // request: it counts as Replies, so the section matches the header.
  const streamed =
    shown && !drawing
      ? Math.max(0, (state?.context_tokens ?? 0) - shown.tokens)
      : 0;
  const tokens = shown ? shown.tokens + streamed : state?.context_tokens;
  const capacity = state?.context_window;
  const [open, setOpen] = useState<Partial<Record<Kind, boolean>>>({});
  const live = !!session?.generation && online;
  const wanted = Object.values(open).some(Boolean);
  const { value: catalogue, error } = useResource(
    () =>
      wanted && live
        ? command("tools", session, { operation: "catalog" }).then(
            (response) => {
              if (response.pending) throw new Error("The list is not ready.");
              return response.result;
            },
          )
        : undefined,
    [session?.id, session?.generation, wanted, live],
  );
  const lists = useMemo(() => {
    const found: Record<Kind, ToolCatalogueItem[]> = {
      tools: [],
      mcp: [],
      hidden: [],
    };
    for (const tool of catalogue?.tools || []) found[kindOf(tool)].push(tool);
    for (const list of Object.values(found))
      list.sort((a, b) => b.schema_bytes - a.schema_bytes);
    return found;
  }, [catalogue]);

  // The host's total spread over the parts by their share of the bytes.
  const scale = shown?.bytes ? shown.tokens / shown.bytes : 0;
  const tokensOf = (part: Part) =>
    (shown?.parts[part] ?? 0) * scale + (part === "replies" ? streamed : 0);
  const share = (part: number) =>
    capacity ? percent((part * 100) / capacity) : "—";
  const compacts = shown && capacity ? shown.compacts_at : 0;
  const mark = capacity ? (compacts * 100) / capacity : 0;
  const usedShare = capacity && tokens ? (tokens * 100) / capacity : 0;
  const missing = error
    ? failure(error).message
    : live
      ? "Loading the list…"
      : "The list shows once this conversation runs.";

  const row = (
    key: string,
    label: string,
    size: number,
    {
      detail,
      fold,
      quiet,
      ratio = share(size),
    }: { detail?: string; fold?: Kind; quiet?: boolean; ratio?: string },
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
          <DataText>{count(Math.round(size))}</DataText>
        </span>
        <span class="muted">
          <DataText>{ratio}</DataText>
        </span>
      </>
    );
    if (!fold)
      return (
        <li key={key} data-quiet={quiet || undefined}>
          <div class="context-row">{body}</div>
        </li>
      );
    // The catalogue measures schemas exactly, the parts as requests do:
    // the list is spread over its row's tokens.
    const tools = lists[fold];
    const each = size / tools.reduce((sum, tool) => sum + tool.schema_bytes, 0);
    return (
      <li key={key} data-quiet={quiet || undefined}>
        <Button
          variant="quiet"
          class="context-row"
          aria-expanded={!!open[fold]}
          disabled={!shown?.tools[fold]?.count}
          onClick={() => setOpen({ ...open, [fold]: !open[fold] })}
        >
          {body}
          <ChevronRight class="row-chevron" />
        </Button>
        {open[fold] && (
          <ul class="context-tools">
            {catalogue ? (
              tools.map((tool) => (
                <li key={tool.name}>
                  <span>{toolName(tool)}</span>
                  {count(Math.round(tool.schema_bytes * each))}
                </li>
              ))
            ) : (
              <li>{missing}</li>
            )}
          </ul>
        )}
      </li>
    );
  };
  const counted = (kind: Kind) => shown?.tools[kind]?.count || 0;
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
            {!drawing &&
              (shown && capacity ? (
                PARTS.map(
                  (part) =>
                    !!tokensOf(part) && (
                      <span
                        key={part}
                        class="context-segment"
                        data-part={part}
                        style={{
                          "--share": `${(tokensOf(part) * 100) / capacity}%`,
                        }}
                      />
                    ),
                )
              ) : (
                <span
                  class="context-segment"
                  style={{ "--share": `${usedShare}%` }}
                />
              ))}
          </div>
          {!!compacts && (
            <span class="context-mark" style={{ "--at": `${mark}%` }} />
          )}
          {shown && (
            <small
              class="context-caption muted"
              data-side={mark > 50 ? "end" : "start"}
              style={{ "--at": `${mark}%` }}
            >
              <DataText>
                {compacts
                  ? `compacts at ${percent(mark)}`
                  : "no automatic compaction"}
              </DataText>
            </small>
          )}
        </div>
      )}
      {shown && (
        <ul class="context-rows">
          {PARTS.map((part) =>
            row(part, LABELS[part], tokensOf(part), {
              detail:
                part === "tools" || part === "mcp"
                  ? String(counted(part))
                  : part === "instructions"
                    ? shown.instructions
                    : undefined,
              fold: part === "tools" || part === "mcp" ? part : undefined,
            }),
          )}
          {(drawing || !!capacity) &&
            row(
              "free",
              drawing || compacts ? "Free until it compacts" : "Free",
              Math.max(0, (compacts || capacity || 0) - (tokens ?? 0)),
              {},
            )}
          {(drawing || !!compacts) &&
            row("kept", "Kept for the summary", (capacity || 0) - compacts, {
              quiet: true,
            })}
          {row(
            "hidden",
            "Not offered",
            (shown.tools.hidden?.bytes || 0) * scale,
            {
              detail: plural(counted("hidden"), "tool"),
              fold: "hidden",
              quiet: true,
              ratio: "—",
            },
          )}
        </ul>
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
            Compact
          </Button>
          <Button disabled={!online} onClick={showContext}>
            Raw context
          </Button>
        </Cluster>
      )}
    </section>
  );
}
