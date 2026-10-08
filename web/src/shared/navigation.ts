import { entry, settled } from "./dismiss.ts";

export const selectedFromURL = () =>
  new URLSearchParams(location.hash.slice(1)).get("session") || "";

// Same-document navigation preserves the shell, connection and browser history.
export function writeSelection(id: string, replace = false) {
  settled(() => {
    const hash = id ? `#session=${encodeURIComponent(id)}` : "";
    if (location.hash === hash) return;
    const url = `${location.pathname}${location.search}${hash}`;
    history[replace ? "replaceState" : "pushState"](entry(), "", url);
  });
}

// A notification's link names the decision a session waits on: once that
// session draws its decision, focus it.
export function focusDecision() {
  const find = () =>
    document.querySelector<HTMLElement>(".conversation .decision h2");
  const observer = new MutationObserver(() => found());
  const timer = setTimeout(() => observer.disconnect(), 15000);
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

// #session=ID&decision=…: open the session and focus its decision. The
// address keeps only the session, so back and reload stay plain.
export function followDecisionLink() {
  if (!new URLSearchParams(location.hash.slice(1)).has("decision")) return;
  writeSelection(selectedFromURL(), true);
  focusDecision();
}
