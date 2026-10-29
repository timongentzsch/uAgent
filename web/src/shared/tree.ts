// The transcript as the host sends it: a list of nodes that name their
// parent, drawn as one tree. A detail level is only how deep the tree starts
// open; nothing here knows a tool, a provider or a model.
import type { NodePatch, Node } from "./types.ts";

// A node and the nodes under it, oldest first.
export interface Branch {
  node: Node;
  kids: Branch[];
}

// The levels a device chooses from, and how deep each opens the tree:
// minimal shows each turn's work as one line, default opens it one level,
// full opens everything.
export const detailLevels = ["minimal", "default", "full"] as const;
export const depthOf = (level: string) =>
  level === "minimal" ? 0 : level === "full" ? Infinity : 1;

// A node with a headline is a row that opens to its body and children; the
// others (your message, a reply, a footer) are drawn whole, with the nodes
// under them after them.
export const isRow = (node: Node) => !!node.headline;

// Whether a row `depth` rows deep starts open at a level `level` deep.
// Routine notices show only when everything does.
export const opens = (depth: number, level: number) => depth < level;
export const visible = (node: Node, level: number) =>
  !node.routine || level === Infinity;

// A row's headline as one line: "Ran npm test".
export const headlineOf = (node: Node) =>
  [node.headline?.verb, node.headline?.target].filter(Boolean).join(" ");

// The newest row under a row: what a closed running row says it is doing.
export function currentStep(branch: Branch): string | undefined {
  const step = branch.kids.findLast((kid) => isRow(kid.node));
  return step && headlineOf(step.node);
}

// Where each node of a list is drawn, in order: a row with what opens under
// it, any other node followed by the nodes under it, at its own level. A
// footer carries its turn's answer, whose time and menu it shows; a running
// row counts from the time of what it is under.
export interface Item {
  branch: Branch;
  answer?: Node;
  since?: string;
}
export function layout(
  branches: Branch[],
  level: number,
  since?: string,
): Item[] {
  const answer = branches.findLast(
    (branch) => branch.node.role === "assistant",
  )?.node;
  return branches
    .filter((branch) => visible(branch.node, level))
    .flatMap((branch) =>
      isRow(branch.node)
        ? [{ branch, since }]
        : [
            {
              branch,
              answer: branch.node.kind === "footer" ? answer : undefined,
            },
            ...layout(branch.kids, level, branch.node.time ?? since),
          ],
    );
}

// A long list of rows: the earlier ones folded into one line, and the
// latest `max` shown.
export function fold(kids: Branch[], max: number): [Branch[], Branch[]] {
  const earlier = Math.max(0, kids.length - max);
  return [kids.slice(0, earlier), kids.slice(earlier)];
}

const NO_KIDS: Branch[] = [];
// A branch is the same object while its node and every node under it are,
// so a streamed frame makes anew only the branches on the changed path.
const grown = new WeakMap<Node, Branch>();

// The tree of `nodes`. A node whose parent is not held yet waits for it:
// the host sends a parent before, or right after, the nodes under it.
export function grow(nodes: Node[]): Branch[] {
  const held = new Set(nodes.map((node) => node.id));
  const under = new Map<string, Node[]>();
  const roots: Node[] = [];
  for (const node of nodes) {
    if (!node.parent) roots.push(node);
    else if (held.has(node.parent) && node.parent !== node.id) {
      const kids = under.get(node.parent);
      if (kids) kids.push(node);
      else under.set(node.parent, [node]);
    }
  }
  const branch = (node: Node): Branch => {
    const made = under.get(node.id)?.map(branch) || NO_KIDS;
    const prior = grown.get(node);
    if (
      prior?.kids.length === made.length &&
      prior.kids.every((kid, index) => kid === made[index])
    )
      return prior;
    const next = { node, kids: made };
    grown.set(node, next);
    return next;
  };
  return roots.map(branch);
}

// One patch applied to the held nodes. A new node goes before the first
// node under it, else in sequence order (a late saved node never strands
// older content after newer), else last: the host's own rule
// (view_reducer.cc), so both hold the same order.
export function placeNode(nodes: Node[], patch: NodePatch): Node[] {
  const id = patch.node?.id ?? patch.id;
  // Streaming patches the newest nodes: search from the end.
  const at = nodes.findLastIndex((node) => node.id === id);
  const next = [...nodes];
  if (patch.node) {
    const node = patch.node;
    if (at >= 0) next[at] = node;
    else {
      const sequence = node.sequence;
      const before = nodes.findIndex(
        (held) =>
          held.parent === node.id ||
          (sequence !== undefined &&
            held.sequence !== undefined &&
            held.sequence > sequence),
      );
      next.splice(before < 0 ? next.length : before, 0, node);
    }
    return next;
  }
  if (at < 0) return nodes;
  const held: Node = { ...nodes[at], ...patch.set };
  if (patch.append?.text && held.body)
    held.body = { ...held.body, text: held.body.text + patch.append.text };
  next[at] = held;
  return next;
}

// At most `max` nodes from the start, cut before a node at the root so no
// turn loses its end: a reader paging back keeps the history around what
// they read, and the newest returns with the next view.
export function keepFirst(nodes: Node[], max: number): Node[] {
  if (nodes.length <= max) return nodes;
  const cut = nodes.findLastIndex((node, at) => at <= max && !node.parent);
  return nodes.slice(0, cut > 0 ? cut : max);
}

// Older nodes in front of newer ones: a page of history before the view,
// or the pages a reader loaded before a checkpoint that replaces the view.
// Where both hold a node, the newer copy wins.
export function mergeNodes(older: Node[], newer: Node[]): Node[] {
  const seen = new Set(newer.map((node) => node.id));
  const kept = older.filter((node) => !seen.has(node.id));
  return kept.length ? [...kept, ...newer] : newer;
}
