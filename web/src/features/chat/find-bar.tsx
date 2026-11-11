import type { RefObject } from "preact";
import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import type { Node, View } from "../../shared/types.ts";
import { FindField } from "../../shared/find-field.tsx";
import { highlights, occurrences, rangesIn } from "../../shared/find.ts";
import { useApi } from "../../state/api.ts";
import { useLatest } from "../../shared/use-latest.ts";

// `at` makes a repeated request a new one.
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

function inView(target: Element | Range, box: Element) {
  const shown = box.getBoundingClientRect();
  const rect = target.getBoundingClientRect();
  return rect.top >= shown.top && rect.bottom <= shown.bottom;
}

// The host counts the rows of the whole conversation; rows this page holds
// that the host has not saved yet are counted here.
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
  root: RefObject<HTMLElement | null>;
  older: () => Promise<void>;
  reveal: (target: Element | Range) => void;
  close: () => void;
}) {
  const { api, routes } = useApi();
  const [words, setWords] = useState(request.words);
  const [saved, setSaved] = useState<{
    words: string;
    rows: { sequence: number; count: number }[];
  }>({ words: "", rows: [] });
  const [current, setCurrent] = useState(-1);
  const target = useRef(request.sequence);
  const loading = useRef(false);
  // A move scrolls to its occurrence once and holds it while a loaded page
  // settles; rows streaming in later only re-mark.
  const moves = useRef(0);
  const revealed = useRef(-1);
  const holdUntil = useRef(0);
  const query = words.trim();

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
  }, [query, request.session, routes.search, api]);

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

  // Lands on the row search chose, else on the newest occurrence.
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

  // The page and its loaders are read as they are, not as a reason to rerun.
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
      // A closed row's own toggle, never a menu button the row also holds.
      const closed = '.node-row[aria-expanded="false"]';
      const folded = row.matches(closed) ? row : row.querySelector(closed);
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
    <FindField
      label="Find in this conversation"
      words={words}
      setWords={setWords}
      count={
        spots.length ||
        (query.length >= 2 && saved.words === query ? 0 : undefined)
      }
      current={Math.max(current, 0)}
      step={step}
      close={close}
    />
  );
}
