import "./node.css";
import { Component, createContext } from "preact";
import type { TargetedKeyboardEvent } from "preact";
import {
  useCallback,
  useContext,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { ChevronRight } from "lucide-preact";
import { Button, ErrorBoundary } from "../../shared/ui.tsx";
import { devicePrefs } from "../../shared/device-prefs.ts";
import {
  maxRememberedToggles,
  maxShownSteps,
  thoughtLines,
} from "../../shared/limits.ts";
import { plural } from "../../shared/quantities.ts";
import { useElapsed } from "../../shared/time.ts";
import { useLatest } from "../../shared/use-latest.ts";
import {
  type Branch,
  currentStep,
  depthOf,
  fold,
  grow,
  headlineOf,
  isRow,
  type Item,
  layout,
  opens,
  visible,
  detailLevels,
} from "../../shared/tree.ts";
import type { Node, SessionRef } from "../../shared/types.ts";
import { NodeBody } from "./bodies.tsx";
import { MessageView } from "./message.tsx";

// How much of the agent's work is drawn: the level, and whether thinking is.
export interface Display {
  detail: string;
  thinking: boolean;
}
export const DetailContext = createContext<Display>({
  detail: detailLevels[0],
  thinking: false,
});

// Rows a person opened or closed here, kept by conversation and node id.
let toggled: Record<string, boolean> | undefined;
devicePrefs.toggles.subscribe((value) => (toggled = value));
const remembered = (key: string) =>
  (toggled ??= devicePrefs.toggles.read())[key];
function remember(key: string, open: boolean) {
  const kept = Object.entries((toggled ??= devicePrefs.toggles.read()))
    .filter(([held]) => held !== key)
    .slice(1 - maxRememberedToggles);
  devicePrefs.toggles.write(Object.fromEntries([...kept, [key, open]]));
}

// The sequences of what a closed row holds, so find can open it to reach one.
const sequencesOf = (branches: Branch[]): string =>
  branches
    .flatMap((branch) => [branch.node.sequence ?? "", sequencesOf(branch.kids)])
    .filter(Boolean)
    .join(" ");

interface Scope {
  // How many rows deep the drawn nodes are, and the level's depth.
  depth: number;
  level: number;
  thinking: boolean;
  online: boolean;
  session: SessionRef;
}

// Keys hold the conversation and level: a new level draws every row afresh,
// and a reused element never carries another conversation's state.
export function Transcript({
  nodes,
  online,
  session,
}: {
  nodes: Node[];
  online: boolean;
  session: SessionRef;
}) {
  const { detail, thinking } = useContext(DetailContext);
  const level = depthOf(detail);
  const roots = useMemo(() => grow(nodes), [nodes]);
  return (
    <Nodes
      items={layout(roots, level, thinking)}
      scope={`${session.id}:${detail}:${thinking}:`}
      depth={0}
      level={level}
      thinking={thinking}
      online={online}
      session={session}
    />
  );
}

// The row this device shows at once and the host's that replaces it share
// a key, so the row is updated in place rather than rebuilt.
const rowKey = (node: Node) =>
  !node.parent && node.request_id ? `request:${node.request_id}` : node.id;

function Nodes({
  items: list,
  scope,
  ...props
}: Scope & { items: Item[]; scope: string }) {
  return (
    <>
      {list.map((item) => (
        <NodeView
          key={`${scope}${rowKey(item.branch.node)}`}
          {...props}
          {...item}
          scope={scope}
        />
      ))}
    </>
  );
}

type NodeProps = Scope & Item & { scope: string };

// One node. A message or footer renders again only when its own node (or a
// footer's answer) changed; a row also when what is under it did.
class NodeView extends Component<NodeProps> {
  shouldComponentUpdate(next: NodeProps) {
    const prior = this.props;
    return (
      (isRow(next.branch.node)
        ? next.branch !== prior.branch
        : next.branch.node !== prior.branch.node) ||
      next.answer !== prior.answer ||
      next.since !== prior.since ||
      next.online !== prior.online ||
      next.session.id !== prior.session.id ||
      next.session.generation !== prior.session.generation
    );
  }
  render(props: NodeProps) {
    const node = props.branch.node;
    return (
      <ErrorBoundary>
        {isRow(node) ? (
          <RowNode {...props} />
        ) : node.kind === "thinking" ? (
          <Thought
            node={node}
            lines={props.level === Infinity ? undefined : thoughtLines}
            online={props.online}
            session={props.session}
          />
        ) : (
          <MessageView
            node={node}
            answer={props.answer}
            online={props.online}
            session={props.session}
          />
        )}
      </ErrorBoundary>
    );
  }
}

// A thought reads as text among the steps, fainter than a reply: whole where
// everything is open, else its first lines and a way to the rest.
function Thought({
  node,
  lines,
  online,
  session,
}: {
  node: Node;
  lines?: number;
  online: boolean;
  session: SessionRef;
}) {
  const [whole, setWhole] = useState(false);
  const [cut, setCut] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const held = !!lines && !whole;
  useLayoutEffect(() => {
    const text = box.current?.querySelector(".node-text");
    if (held && text) setCut(text.scrollHeight > text.clientHeight + 1);
  }, [held, node]);
  return (
    <div
      ref={box}
      class="node thought"
      data-kind="thinking"
      data-message-id={node.id}
      data-sequence={node.sequence}
      data-held={held || undefined}
      style={held ? { "--lines": lines } : undefined}
    >
      <NodeBody node={node} online={online} session={session} />
      {!!lines && (cut || whole) && (
        <Button
          variant="quiet"
          class="node-action small"
          aria-expanded={whole}
          onClick={() => setWhole(!whole)}
        >
          {whole ? "Less" : "More"}
        </Button>
      )}
    </div>
  );
}

// Starts open when less deep than the level, unless this device remembers
// the person opening or closing it.
function RowNode({
  branch,
  since,
  depth,
  level,
  thinking,
  online,
  session,
  scope,
}: NodeProps) {
  const node = branch.node;
  const key = `${session.id}:${node.id}`;
  const [open, setOpen] = useState(
    () => remembered(key) ?? opens(depth, level),
  );
  const latest = useLatest({ open, key });
  const toggle = useCallback((next?: boolean) => {
    const value = next ?? !latest.current.open;
    if (value === latest.current.open) return;
    setOpen(value);
    remember(latest.current.key, value);
  }, []);
  const kids = branch.kids.filter((kid) => visible(kid.node, level, thinking));
  // What the agent says while it works shows as it says it, whatever is
  // folded; once the turn has ended it is among the work's steps.
  const said =
    !open && node.kind === "work" && node.state === "running"
      ? kids.filter((kid) => kid.node.role === "assistant")
      : [];
  // So does its latest thought, on one line, while it is the latest thing.
  const working = !open && node.kind === "work" && node.state === "running";
  const thought =
    working && kids.at(-1)?.node.kind === "thinking"
      ? kids.at(-1)?.node
      : undefined;
  const openable =
    !!node.body ||
    kids.length > 0 ||
    !!node.input?.length ||
    !!node.parts?.length ||
    node.kind === "call";
  return (
    <div
      class={`node ${node.state || ""}`}
      data-kind={node.kind}
      data-message-id={node.id}
      data-sequence={node.sequence}
      data-sequences={(!open && sequencesOf(kids)) || undefined}
    >
      <NodeRow
        node={node}
        open={open}
        openable={openable}
        toggle={toggle}
        thinking={thinking}
        step={!open && node.state === "running" ? currentStep(branch) : ""}
        since={node.time ?? since}
      />
      {said.length > 0 && (
        <Nodes
          items={said.map((kid) => ({ branch: kid }))}
          depth={depth + 1}
          level={level}
          thinking={thinking}
          online={online}
          session={session}
          scope={scope}
        />
      )}
      {thought && (
        <Thought node={thought} lines={1} online={online} session={session} />
      )}
      {open && openable && (
        <div class="node-open">
          <NodeBody node={node} online={online} session={session} />
          {kids.length > 0 && (
            <Kids
              kids={kids}
              since={node.time ?? since}
              depth={depth + 1}
              level={level}
              thinking={thinking}
              online={online}
              session={session}
              scope={scope}
            />
          )}
        </div>
      )}
    </div>
  );
}

// A long list shows its latest rows and one line that opens the earlier ones.
function Kids({
  kids,
  since,
  scope,
  ...props
}: Scope & {
  kids: Branch[];
  since?: string;
  scope: string;
}) {
  const [all, setAll] = useState(false);
  const [earlier, shown] = all ? [[], kids] : fold(kids, maxShownSteps);
  return (
    <div class="node-kids">
      {earlier.length > 0 && (
        <Button
          variant="quiet"
          class="node-row"
          aria-expanded="false"
          data-sequences={sequencesOf(earlier)}
          onClick={() => setAll(true)}
        >
          <ChevronRight class="row-chevron" aria-hidden="true" />
          <span class="node-headline">
            … {plural(earlier.length, "earlier step")}
          </span>
        </Button>
      )}
      <Nodes
        items={layout(shown, props.level, props.thinking, since)}
        scope={scope}
        {...props}
      />
    </div>
  );
}

// ←/→ close or open a row, or move to the row it is under or the first
// under it, as in a tree.
function arrows(
  event: TargetedKeyboardEvent<HTMLButtonElement>,
  open: boolean,
  toggle: (next?: boolean) => void,
) {
  const row = event.currentTarget;
  const node = row.parentElement;
  let target: Element | null | undefined;
  if (event.key === "ArrowRight") {
    if (!open) toggle(true);
    else target = node?.querySelector(".node-kids .node-row");
  } else if (event.key === "ArrowLeft") {
    if (open) toggle(false);
    else
      target = node?.parentElement
        ?.closest(".node")
        ?.querySelector(":scope > .node-row");
  } else return;
  event.preventDefault();
  if (target instanceof HTMLElement) target.focus();
}

class NodeRow extends Component<{
  node: Node;
  open: boolean;
  openable: boolean;
  toggle: (next?: boolean) => void;
  thinking: boolean;
  step: string | undefined;
  since?: string;
}> {
  shouldComponentUpdate(next: NodeRow["props"]) {
    const prior = this.props;
    return (
      next.node !== prior.node ||
      next.open !== prior.open ||
      next.openable !== prior.openable ||
      next.thinking !== prior.thinking ||
      next.step !== prior.step ||
      next.since !== prior.since
    );
  }
  render() {
    return <RowLine {...this.props} />;
  }
}

function RowLine({
  node,
  open,
  openable,
  toggle,
  thinking,
  step,
  since,
}: NodeRow["props"]) {
  const elapsed = useElapsed(
    node.state === "running" && node.kind !== "work",
    since ? Date.parse(since) : undefined,
  );
  const headline = [headlineOf(node, thinking), step]
    .filter(Boolean)
    .join(" · ");
  const word = [node.result?.word, elapsed].filter(Boolean).join(" · ");
  const content = (
    <>
      <ChevronRight class="row-chevron" aria-hidden="true" />
      <span
        class={
          node.state === "running" ? "node-headline working" : "node-headline"
        }
        title={headline}
      >
        {node.state === "failed" && <span class="sr-only">Failed: </span>}
        {headline}
      </span>
      {word && <small class="node-word">{word}</small>}
    </>
  );
  return openable ? (
    <Button
      variant="quiet"
      class="node-row"
      aria-expanded={open}
      onClick={() => toggle()}
      onKeyDown={(event) => arrows(event, open, toggle)}
    >
      {content}
    </Button>
  ) : (
    <div class="node-row">{content}</div>
  );
}
