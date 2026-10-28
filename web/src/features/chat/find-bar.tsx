import { ChevronDown, ChevronUp, X } from "lucide-preact";
import type { RefObject } from "preact";
import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import type { Node, View } from "../../shared/types.ts";
import { IconButton, Input } from "../../shared/ui.tsx";
import { highlights, occurrences, rangesIn } from "../../shared/find.ts";
import { api, routes } from "../../state/api.ts";
import { useLatest } from "../../shared/use-latest.ts";

// What opens the find bar: the conversation, its words, and the row to
// start at when search opened it there. `at` makes a repeat a new request.
export type FindRequest = {
  session: string;
  words: string;
  sequence?: number;
  at: number;
};

// One occurrence: the row it is in and which of that row's it is.
type Spot = { sequence: number; nth: number };

const ASK_AFTER_MS = 200;
const HOLD_MS = 600;

// The node of a sequence, or the closed row that holds it.
const rowOf = (root: Element, sequence: number) =>
  root.querySelector(`[data-sequence="${sequence}"]`) ||
  root.querySelector(`[data-sequences~="${sequence}"]`);

// Whether a range or row shows inside the transcript's box.
function inView(target: Element | Range, box: Element) {
  const shown = box.getBoundingClientRect();
  const rect = target.getBoundingClientRect();
  return rect.top >= shown.top && rect.bottom <= shown.bottom;
}

// Find in the open conversation, as a PDF viewer finds: every occurrence
// marked, the current one stronger, Enter and Shift+Enter step down and
// up through all of them. The host counts the rows of the whole
// conversation; rows this page holds that the host has not saved yet are
// counted here.
export function FindBar({
  request,
  view,
  nodes,
  root,
  older,
  reveal,
  close,
}: {
  request: FindRequest;
  view?: View;
  nodes: Node[];
  // The transcript, whose text is searched and marked.
  root: RefObject<HTMLElement | null>;
  // Loads the page of rows before the oldest one shown.
  older: () => Promise<void>;
  // Scrolls a row or an occurrence into view.
  reveal: (target: Element | Range) => void;
  close: () => void;
}) {
  const [words, setWords] = useState(request.words);
  const [saved, setSaved] = useState<{
    words: string;
    rows: { sequence: number; count: number }[];
  }>({ words: "", rows: [] });
  const [current, setCurrent] = useState(-1);
  // A row to land on once the counts arrive (from search), and a page of
  // older rows on its way.
  const target = useRef(request.sequence);
  const loading = useRef(false);
  // A move to an occurrence scrolls to it once, and keeps it in view for a
  // moment while the transcript settles a page it loaded around it; rows
  // streaming in later only re-mark, so they do not pull the view back.
  const moves = useRef(0);
  const revealed = useRef(-1);
  const holdUntil = useRef(0);
  const bar = useRef<HTMLDivElement>(null);
  const query = words.trim();

  // A frame later: a menu that opened the bar gives focus back to its
  // button as it closes.
  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      const field = bar.current?.querySelector("input");
      field?.focus({ preventScroll: true });
      field?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, [request.at]);

  // The host reads the saved conversation once typing pauses.
  useEffect(() => {
    if (query.length < 2) return;
    const controller = new AbortController();
    const timer = setTimeout(() => {
      api<{ hits: { sequence: number; count: number }[] }>(
        routes.search(query, request.session),
        undefined,
        { signal: controller.signal },
      )
        .then((value) => setSaved({ words: query, rows: value.hits }))
        .catch(() => {});
    }, ASK_AFTER_MS);
    return () => {
      clearTimeout(timer);
      controller.abort();
    };
  }, [query, request.session]);

  const spots = useMemo(() => {
    if (query.length < 2 || saved.words !== query) return [];
    const counts = new Map(saved.rows.map((row) => [row.sequence, row.count]));
    for (const node of nodes)
      if (node.sequence && !counts.has(node.sequence)) {
        const count = occurrences(node.body?.text || "", query).length;
        if (count) counts.set(node.sequence, count);
      }
    return [...counts]
      .sort(([a], [b]) => a - b)
      .flatMap(([sequence, count]) =>
        Array.from({ length: count }, (_, nth) => ({ sequence, nth })),
      );
  }, [saved, nodes, query]);

  // New words counted: land on the row search chose, else on the newest
  // occurrence, which is nearest the end a conversation is read from. Rows
  // that stream in later are added after it and do not move it.
  // biome-ignore lint/correctness/useExhaustiveDependencies: lands once per host answer; rows streaming in must not move the current occurrence
  useEffect(() => {
    if (!spots.length) return setCurrent(-1);
    const wanted = target.current;
    target.current = undefined;
    const at =
      wanted === undefined
        ? -1
        : spots.findIndex((spot) => spot.sequence >= wanted);
    moves.current++;
    setCurrent(at >= 0 ? at : spots.length - 1);
  }, [saved]);

  // Marks every occurrence the transcript shows and brings the current one
  // into view, opening a closed row or loading an older page first. The
  // page and its loaders are read as they are now, not as a reason to
  // look again.
  const page = useLatest({ view, older, reveal });
  useEffect(() => {
    const { view, older, reveal } = page.current;
    const element = root.current;
    if (!element) return;
    // Without the Highlight API the bar still scrolls to each occurrence.
    const marks = highlights;
    const spot: Spot | undefined = spots[current];
    let frame = 0;
    const show = () => {
      const all = query.length >= 2 ? rangesIn(element, query) : [];
      marks?.set("find", new Highlight(...all));
      marks?.delete("find-current");
      if (!spot) return;
      const row = rowOf(element, spot.sequence);
      if (!row) {
        const before = view?.before;
        if (
          view?.more &&
          !loading.current &&
          (!before || spot.sequence < before)
        ) {
          loading.current = true;
          void older().finally(() => (loading.current = false));
        }
        return;
      }
      const ranges = rangesIn(row, query);
      const folded = row.matches('[aria-expanded="false"]')
        ? row
        : row.querySelector('[aria-expanded="false"]');
      if (ranges.length <= spot.nth && folded instanceof HTMLElement) {
        // A closed row mounts its body on opening; look again after it.
        folded.click();
        frame = requestAnimationFrame(show);
        return;
      }
      const range = ranges[Math.min(spot.nth, ranges.length - 1)];
      const found = range || row;
      if (range) marks?.set("find-current", new Highlight(range));
      if (revealed.current !== moves.current) {
        revealed.current = moves.current;
        holdUntil.current = performance.now() + HOLD_MS;
      } else if (performance.now() > holdUntil.current) return;
      if (!inView(found, element)) reveal(found);
      frame = requestAnimationFrame(show);
    };
    show();
    return () => cancelAnimationFrame(frame);
  }, [spots, current, nodes, query, root, page]);

  useEffect(
    () => () => {
      highlights?.delete("find");
      highlights?.delete("find-current");
    },
    [],
  );

  const step = (by: number) => {
    if (!spots.length) return;
    moves.current++;
    setCurrent((at) => (Math.max(at, 0) + by + spots.length) % spots.length);
  };

  return (
    <div
      class="find-bar"
      role="search"
      aria-label="Find in this conversation"
      ref={bar}
    >
      <Input
        type="search"
        aria-label="Find in this conversation"
        placeholder="Find in this conversation…"
        value={words}
        onInput={(event) => setWords(event.currentTarget.value)}
        onKeyDown={(event) => {
          if (event.isComposing) return;
          if (event.key === "Enter") step(event.shiftKey ? -1 : 1);
          else if (event.key === "Escape") close();
          else return;
          event.preventDefault();
          event.stopPropagation();
        }}
      />
      <span class="find-count" aria-live="polite">
        {spots.length
          ? `${Math.max(current, 0) + 1} of ${spots.length}`
          : query.length >= 2 && saved.words === query
            ? "No matches"
            : ""}
      </span>
      <IconButton
        label="Previous match"
        disabled={!spots.length}
        onClick={() => step(-1)}
      >
        <ChevronUp />
      </IconButton>
      <IconButton
        label="Next match"
        disabled={!spots.length}
        onClick={() => step(1)}
      >
        <ChevronDown />
      </IconButton>
      <IconButton label="Close find" onClick={close}>
        <X />
      </IconButton>
    </div>
  );
}
