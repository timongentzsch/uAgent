import { useEffect, useRef, useState } from "preact/hooks";
import { Button } from "../../shared/ui.tsx";
import Frame, {
  type PageInfo,
  pageAddress,
  pageSize,
  usePreviewBase,
} from "../../shared/page-frame.tsx";
import { useLatest } from "../../shared/use-latest.ts";
import { type Presenting, presentChannel } from "./present.ts";
import "./files.css";

// The presenter's own window: the slide now and the next, the speaker notes
// and a clock. Its keys and buttons move the deck's window too.

// One slide, as large as its box allows.
function Slide({
  base,
  path,
  onInfo,
}: {
  base: string;
  path: string;
  onInfo?: (info: PageInfo) => void;
}) {
  const box = useRef<HTMLDivElement>(null);
  const [room, setRoom] = useState({ w: 0, h: 0 });
  const [info, setInfo] = useState<PageInfo>();
  useEffect(() => {
    const observer = new ResizeObserver(([entry]) =>
      setRoom({ w: entry.contentRect.width, h: entry.contentRect.height }),
    );
    if (box.current) observer.observe(box.current);
    return () => observer.disconnect();
  }, []);
  const { w, h } = pageSize(info, 1080);
  const scale = Math.min(room.w / w, room.h / h) || 0;
  return (
    <div class="presenter-slide" ref={box}>
      <Frame
        key={path}
        src={pageAddress(base, path)}
        title={info?.title || path}
        width={w}
        height={h}
        scale={scale}
        live={false}
        onMessage={(message) => {
          if (message.type !== "ready") return;
          setInfo(message);
          onInfo?.(message);
        }}
      />
    </div>
  );
}

const clock = (ms: number) => {
  const seconds = Math.floor(ms / 1000);
  return `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
};

export default function Presenter({ deck }: { deck: Presenting }) {
  const base = usePreviewBase(deck.session).value?.base;
  const [at, setAt] = useState(deck.at);
  const [notes, setNotes] = useState("");
  const [started, setStarted] = useState(() => Date.now());
  const [now, setNow] = useState(() => Date.now());
  const channel = useRef<BroadcastChannel>(undefined);
  useEffect(() => {
    const opened = presentChannel();
    opened.onmessage = (event) =>
      deck.slides.includes(event.data?.at) && setAt(event.data.at);
    channel.current = opened;
    const tick = setInterval(() => setNow(Date.now()), 1000);
    return () => {
      opened.close();
      clearInterval(tick);
    };
  }, [deck]);
  const index = deck.slides.indexOf(at);
  const go = useLatest((by: number) => {
    const next = deck.slides[index + by];
    if (!next) return;
    setAt(next);
    channel.current?.postMessage({ at: next });
  });
  useEffect(() => {
    const keys = (event: KeyboardEvent) => {
      if (["ArrowRight", " ", "PageDown"].includes(event.key)) go.current(1);
      if (["ArrowLeft", "PageUp"].includes(event.key)) go.current(-1);
    };
    addEventListener("keydown", keys);
    return () => removeEventListener("keydown", keys);
  }, [go]);
  useEffect(() => setNotes(""), [at]);
  const next = deck.slides[index + 1];
  return (
    <main class="presenter" aria-label="Presenter view">
      <header class="presenter-bar">
        <span>
          Slide {index + 1} of {deck.slides.length}
        </span>
        <span class="presenter-clock" role="timer">
          {clock(now - started)}
        </span>
        <Button variant="secondary" onClick={() => setStarted(Date.now())}>
          Reset
        </Button>
      </header>
      {base && (
        <div class="presenter-body">
          <section class="presenter-now" aria-label="Now">
            <Slide
              base={base}
              path={at}
              onInfo={(info) => setNotes(info.notes || "")}
            />
          </section>
          <section class="presenter-side" aria-label="Next and notes">
            <span class="small muted">Next</span>
            {next ? (
              <Slide base={base} path={next} />
            ) : (
              <p class="presenter-end small muted">End of the deck</p>
            )}
            <span class="small muted">Notes</span>
            <p class="presenter-notes">{notes || "No notes."}</p>
          </section>
        </div>
      )}
      <footer class="presenter-bar">
        <Button
          variant="secondary"
          disabled={index <= 0}
          onClick={() => go.current(-1)}
        >
          Back
        </Button>
        <Button
          variant="primary"
          disabled={!next}
          onClick={() => go.current(1)}
        >
          Next
        </Button>
      </footer>
    </main>
  );
}
