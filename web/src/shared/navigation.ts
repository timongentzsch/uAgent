import { entry, settled, underLayers } from "./dismiss.ts";
import { decisionFocusWaitMs } from "./limits.ts";

const addressParams = () => new URLSearchParams(location.hash.slice(1));

export const hostFromURL = () => addressParams().get("host") || "";

export const selectedFromURL = () => addressParams().get("session") || "";

function selectionParams(id: string, host: string) {
  const params = new URLSearchParams();
  if (host) params.set("host", host);
  if (id) params.set("session", id);
  return params;
}

function workspacePath(path: string) {
  return (
    path &&
    !/[\\\u0000-\u001f]/.test(path) &&
    !/^[a-z]:\//i.test(path) &&
    path.split("/").every((part) => part && part !== "." && part !== "..")
  );
}

export function artifactURL(
  session: string,
  host: string,
  path: string,
  origin?: string,
) {
  if (!session || !workspacePath(path)) return undefined;
  const params = selectionParams(session, host);
  params.set("file", path);
  if (origin) params.set("origin", origin);
  return `${location.origin}${location.pathname}#${params}`;
}

export const fileOriginFromURL = () =>
  addressParams().get("origin") ?? undefined;

export function fileFromURL() {
  const path = addressParams().get("file") ?? "";
  return workspacePath(path) ? path : "";
}

// Follow navigation inside a linked viewer without adding a history entry
// on every slide. A normal Files pane leaves the conversation URL alone.
export function replaceLinkedFile(id: string, previous: string, path = "") {
  if (selectedFromURL() !== id || fileFromURL() !== previous) return false;
  const params = addressParams();
  if (path && workspacePath(path)) params.set("file", path);
  else {
    params.delete("file");
    params.delete("origin");
  }
  history.replaceState(history.state, "", `#${params}`);
  return true;
}

// A linked phone sheet owns its file address. Back lands on the conversation.
export function withoutFileAddress() {
  const params = addressParams();
  if (!params.has("file")) return undefined;
  params.delete("file");
  params.delete("origin");
  return `#${params}`;
}

// #settings=approvals/rules: Settings open at a topic and row, kept in the
// address while it is open, so a reload or a link opens the same place.
export const settingsFromURL = () =>
  addressParams().get("settings") ?? undefined;

const hashOf = (params: URLSearchParams) =>
  `#${params.toString().replace(/%2F/g, "/")}`;

export function writeSettingsAddress(address: string) {
  const params = addressParams();
  params.set("settings", address);
  history.replaceState(history.state, "", hashOf(params));
}

// What Back from a linked Settings lands on.
export function withoutSettingsAddress() {
  const params = addressParams();
  if (!params.has("settings")) return undefined;
  params.delete("settings");
  return hashOf(params);
}

// In an iOS home-screen app the left edge opens the drawer, and Safari
// swipes back from that edge whenever there is an entry to go back to. So
// there a conversation replaces the last one, written under any open layer.
const edgeOpensDrawer =
  (navigator as { standalone?: boolean }).standalone === true;

// Same-document navigation keeps the shell, connection and browser history.
export function writeSelection(
  id: string,
  replace = false,
  host = hostFromURL(),
) {
  (edgeOpensDrawer ? underLayers : settled)(() => {
    const params = selectionParams(id, host);
    const hash = params.size ? `#${params}` : "";
    if (location.hash === hash) return;
    const url = `${location.pathname}${location.search}${hash}`;
    const keep = replace || edgeOpensDrawer;
    history[keep ? "replaceState" : "pushState"](entry(), "", url);
  });
}

// Focuses the message input once drawn, which a just chosen session delays.
export function focusComposer(tries = 10) {
  requestAnimationFrame(() => {
    const prompt = document.getElementById("prompt");
    if (prompt) prompt.focus({ preventScroll: true });
    else if (tries) focusComposer(tries - 1);
  });
}

// A notification's link names the decision a session waits on: once that
// session draws its decision, focus it.
export function focusDecision() {
  const find = () =>
    document.querySelector<HTMLElement>(".conversation .decision h2");
  const observer = new MutationObserver(() => found());
  const timer = setTimeout(() => observer.disconnect(), decisionFocusWaitMs);
  const found = () => {
    const heading = find();
    if (!heading) return false;
    observer.disconnect();
    clearTimeout(timer);
    heading.scrollIntoView({ block: "nearest" });
    heading.focus({ preventScroll: true });
    return true;
  };
  // After this frame: the conversation it leaves may still be drawn.
  requestAnimationFrame(() => {
    if (!found())
      observer.observe(document.body, { childList: true, subtree: true });
  });
}

export function revealLiveRow() {
  const rows = document.querySelectorAll<HTMLElement>(
    ".transcript .node.running > .node-row",
  );
  const row = rows[rows.length - 1];
  row?.scrollIntoView({ block: "center" });
  row?.focus({ preventScroll: true });
}

// #session=ID&decision=…: open the session and focus its decision. The
// address keeps only the session, so back and reload stay plain.
export function followDecisionLink() {
  if (!addressParams().has("decision")) return;
  writeSelection(selectedFromURL(), true);
  focusDecision();
}
