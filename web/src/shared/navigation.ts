import { entry, settled, underLayers } from "./dismiss.ts";
import { decisionFocusWaitMs } from "./limits.ts";

export const hostFromURL = () =>
  new URLSearchParams(location.hash.slice(1)).get("host") || "";

export const selectedFromURL = () =>
  new URLSearchParams(location.hash.slice(1)).get("session") || "";

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
    const params = new URLSearchParams();
    if (host) params.set("host", host);
    if (id) params.set("session", id);
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
  if (!new URLSearchParams(location.hash.slice(1)).has("decision")) return;
  writeSelection(selectedFromURL(), true);
  focusDecision();
}
