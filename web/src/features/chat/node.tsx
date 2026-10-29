import "./node.css";
import { Component, createContext } from "preact";
import type { TargetedKeyboardEvent } from "preact";
import { useCallback, useContext, useMemo, useState } from "preact/hooks";
import { ChevronRight } from "lucide-preact";
import { Button, ErrorBoundary } from "../../shared/ui.tsx";
import { devicePrefs } from "../../shared/device-prefs.ts";
import { maxRememberedToggles, maxShownSteps } from "../../shared/limits.ts";
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
} from "../../shared/tree.ts";
import type { Node, SessionRef } from "../../shared/types.ts";
import { NodeBody } from "./bodies.tsx";
import { MessageView } from "./message.tsx";

// The detail level transcripts start at on this device.
export const DetailContext = createContext("default");

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
  online: boolean;
  session: SessionRef;
}

// The transcript: the host's nodes as one tree, open to the level's depth.
// Each node is drawn by its own component, which renders again only when its
// node, or for a row what is under it, changed: a streamed frame renders the
// one node it touched. Keys hold the conversation and level: a new level
// draws every row from its own start, and a reused element never carries
// another conversation's state for the same id.
export function Transcript({
  nodes,
  online,
  session,
}: {
  nodes: Node[];
  online: boolean;
  session: SessionRef;
}) {
  const detail = useContext(DetailContext);
  const level = depthOf(detail);
  const roots = useMemo(() => grow(nodes), [nodes]);
  return (
    <Nodes
      items={layout(roots, level)}
      scope={`${session.id}:${detail}:`}
      depth={0}
      level={level}
      online={online}
      session={session}
    />
  );
}

function Nodes({
  items: list,
  scope,
  ...props
}: Scope & { items: Item[]; scope: string }) {
  return (
    <>
      {list.map((item) => (
        <NodeView
          key={`${scope}${item.branch.node.id}`}
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

// A row: one line that opens to its body and the rows under it. It starts
// open when it is less deep than the level, unless this device remembers
// the person opening or closing it.
function RowNode({
  branch,
  since,
  depth,
  level,
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
  const kids = branch.kids.filter((kid) => visible(kid.node, level));
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
        step={!open && node.state === "running" ? currentStep(branch) : ""}
        since={node.time ?? since}
      />
      {open && openable && (
        <div class="node-open">
          <NodeBody node={node} online={online} session={session} />
          {kids.length > 0 && (
            <Kids
              kids={kids}
              unit={node.result?.unit}
              since={node.time ?? since}
              depth={depth + 1}
              level={level}
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

// The rows under an open row. A long list shows its latest rows and one
// line that opens the earlier ones.
function Kids({
  kids,
  unit = "step",
  since,
  scope,
  ...props
}: Scope & {
  kids: Branch[];
  unit?: string;
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
            … {plural(earlier.length, `earlier ${unit}`)}
          </span>
        </Button>
      )}
      <Nodes
        items={layout(shown, props.level, since)}
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

// A row's one line: its headline, then its result word. A failed row reads
// red, a running one shimmers and counts its time, and a closed running row
// says what it is doing now.
class NodeRow extends Component<{
  node: Node;
  open: boolean;
  openable: boolean;
  toggle: (next?: boolean) => void;
  step: string | undefined;
  since?: string;
}> {
  shouldComponentUpdate(next: NodeRow["props"]) {
    const prior = this.props;
    return (
      next.node !== prior.node ||
      next.open !== prior.open ||
      next.openable !== prior.openable ||
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
  step,
  since,
}: NodeRow["props"]) {
  const elapsed = useElapsed(
    node.state === "running",
    since ? Date.parse(since) : undefined,
  );
  const headline = [headlineOf(node), step].filter(Boolean).join(" · ");
  const word = [node.result?.word, elapsed].filter(Boolean).join(" · ");
  const content = (
    <>
      <ChevronRight class="row-chevron" aria-hidden="true" />
      <span class="node-headline" title={headline}>
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
