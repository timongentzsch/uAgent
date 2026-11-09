// Presenting a deck in two windows: the slides in one, the presenter's view
// (now, next, notes, a clock) in another. One BroadcastChannel keeps them on
// the same slide; either moves both.

export interface Presenting {
  session: string;
  // The deck's slides in order, by path in the conversation's folder.
  slides: string[];
  at: string;
}

const HASH = "#presenter=";
export const presentChannel = () => new BroadcastChannel("uagent-present");

// Opens the presenter's view in a window of its own.
export function openPresenter(deck: Presenting) {
  window.open(
    `${location.pathname}${HASH}${encodeURIComponent(JSON.stringify(deck))}`,
    "uagent-presenter",
    "popup,width=1280,height=800",
  );
}

// The deck this window presents for, when it is a presenter's view.
export function presenterDeck(): Presenting | null {
  if (!location.hash.startsWith(HASH)) return null;
  try {
    const deck = JSON.parse(
      decodeURIComponent(location.hash.slice(HASH.length)),
    );
    return typeof deck?.session === "string" &&
      Array.isArray(deck.slides) &&
      deck.slides.includes(deck.at)
      ? deck
      : null;
  } catch {
    return null;
  }
}
