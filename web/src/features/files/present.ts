// Presenting a deck in two windows, the slides and the presenter's view: one
// BroadcastChannel keeps them on the same slide, and either moves both.
import { useEffect, useRef } from "preact/hooks";
import { useLatest } from "../../shared/use-latest.ts";
import { requestId } from "../../state/api.ts";

export interface Presenting {
  session: string;
  channel?: string;
  // The deck's slides in order, by path in the conversation's folder.
  slides: string[];
  at: string;
}

// What a key steps a deck by, in either window.
export const STEPS: Record<string, number | undefined> = {
  ArrowRight: 1,
  PageDown: 1,
  " ": 1,
  ArrowLeft: -1,
  PageUp: -1,
};

const HASH = "#presenter=";

// The slide shown, and the build shown on it.
type Said = { at?: string; build?: number };
// One pair per presenting tab, even when two tabs open the same slide.
let stageChannel: string | undefined;
export const presenterChannel = () => (stageChannel ??= requestId());

export function usePresentChannel(
  id: string | undefined,
  heard: (said: Said) => void,
) {
  const latest = useLatest(heard);
  const current = useRef<BroadcastChannel>(null);
  useEffect(() => {
    if (!id) return;
    const channel = new BroadcastChannel(`uagent-present-${id}`);
    current.current = channel;
    channel.onmessage = (event: MessageEvent<Said>) =>
      latest.current(event.data ?? {});
    return () => {
      channel.close();
      current.current = null;
    };
  }, [id, latest]);
  return (said: Said) => current.current?.postMessage(said);
}

// Opened in the press that asks for it; it goes to the deck once `ready`
// (what was typed in a slide is in its file).
export function openPresenter(deck: Presenting, ready: Promise<unknown>) {
  const channel = presenterChannel();
  const made = window.open(
    "",
    `uagent-presenter-${channel}`,
    "popup,width=1280,height=800",
  );
  const address = new URL(
    `${location.pathname}${HASH}${encodeURIComponent(JSON.stringify({ ...deck, channel }))}`,
    location.href,
  );
  void ready.then(() => made?.location.replace(address));
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
