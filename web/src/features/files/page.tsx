import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import {
  Copy,
  ExternalLink,
  ImageDown,
  PenLine,
  MousePointerClick,
  Presentation,
  Printer,
  SlidersHorizontal,
} from "lucide-preact";
import { Button, IconButton, Switch } from "../../shared/ui.tsx";
import { Input, Textarea } from "../../shared/form-controls.tsx";
import Frame, {
  type PageInfo,
  type Pick,
  type Rect,
  pageSize,
  tell,
} from "../../shared/page-frame.tsx";
import { readTokens } from "./tokens.ts";
import { ZoomControls } from "../../shared/zoom-surface.tsx";

// The zoom of an opened page, as a scale of its real size, and one step.
const ZOOM_MIN = 0.1;
const ZOOM_MAX = 4;
const ZOOM_STEP = 1.25;
import { useResource } from "../../shared/use-resource.ts";

// A page opened across the pane: a fixed artboard scaled to fit, a page at
// the pane's own width. Select picks an element to comment on; Tweaks tries
// the levers the page declares; Present shows it full screen, stepping
// through its folder; Open and PDF show it alone in a tab.

interface Tweak {
  editor?: string;
  default?: unknown;
  options?: unknown[];
  min?: number;
  max?: number;
  step?: number;
  // Shown beside the value, never applied: the page's CSS gives the unit.
  unit?: string;
}

// The lines it adds to the draft, which the design skill reads.
const commentLine = (path: string, pick: Pick, comment: string) =>
  `\`${path}\` \`${pick.selector}\`${pick.text ? ` “${pick.text}”` : ""} — ${comment}`;
const tweaksLine = (path: string, values: Record<string, unknown>) =>
  `\`${path}\` tweaks: ${Object.entries(values)
    .map(([name, value]) => `${name} ${value}`)
    .join(", ")}`;

export default function OpenPage({
  path,
  src,
  siblings,
  go,
  addToChat,
  base,
  read,
  picture,
  view,
}: {
  path: string;
  src: string;
  // The folder's preview address, and a file of the folder as text: a
  // design system the page links shows under Theme.
  base: string;
  read: (path: string) => Promise<string>;
  // The page drawn as a PNG by the agent's browser, where there is one.
  picture?: (width: number, height: number, scale: number) => string;
  // Opens a picture in the image viewer, which can draw on it.
  view: (image: { src: string; name: string }) => void;
  // The pages of its folder in canvas order, which Present steps through.
  siblings: string[];
  go: (path: string) => void;
  addToChat?: (text: string, keep?: boolean) => void;
}) {
  const frame = useRef<HTMLIFrameElement>(null);
  const stage = useRef<HTMLDivElement>(null);
  const scroller = useRef<HTMLDivElement>(null);
  const [room, setRoom] = useState({ w: 0, h: 0 });
  // What the page said it is, for the address it said it at.
  const [said, setSaid] = useState<PageInfo & { src: string }>();
  const info = said?.src === src ? said : undefined;
  const [selecting, setSelecting] = useState(false);
  const [hover, setHover] = useState<Rect>();
  const [picked, setPicked] = useState<Pick>();
  const [comment, setComment] = useState("");
  const [tweaking, setTweaking] = useState(false);
  const [values, setValues] = useState<Record<string, unknown>>({});
  // Reset reloads the page: what it declares comes back.
  const [load, setLoad] = useState(0);
  useEffect(() => {
    const observer = new ResizeObserver(([entry]) =>
      setRoom({ w: entry.contentRect.width, h: entry.contentRect.height }),
    );
    if (scroller.current) observer.observe(scroller.current);
    return () => observer.disconnect();
  }, []);
  // Another page: nothing picked or tried carries over.
  useEffect(() => {
    setPicked(undefined);
    setHover(undefined);
    setValues({});
  }, [src]);
  const step = (by: number) => {
    const at = siblings.indexOf(path);
    const next = siblings[(at + by + siblings.length) % siblings.length];
    if (at >= 0 && next !== path) go(next);
  };
  useEffect(() => {
    const keys = (event: KeyboardEvent) => {
      if (!document.fullscreenElement) return;
      if (event.key === "ArrowRight") step(1);
      if (event.key === "ArrowLeft") step(-1);
    };
    document.addEventListener("keydown", keys);
    return () => document.removeEventListener("keydown", keys);
  });
  // Its size as a picture: an artboard's, or a page's full height.
  const size = pageSize(info, 8000);
  const { fixed } = size;
  // A fixed artboard fits the stage whole; a page that names only its width
  // (an A4 document) fits it across and scrolls down inside, as a PDF does.
  const wide = !!info?.artboard.w;
  const fit = !room.w
    ? 1
    : fixed
      ? Math.min(1, room.w / info!.artboard.w, room.h / info!.artboard.h)
      : wide
        ? Math.min(1, room.w / info!.artboard.w)
        : 1;
  // How far the person zoomed from the fit; each page opens at the fit.
  const [zoom, setZoom] = useState(1);
  useEffect(() => setZoom(1), [src]);
  const scale = fit * zoom;
  // Scales by `by` keeping the point (x, y) of the scroller where it is.
  const anchor = useRef<{ x: number; y: number; cx: number; cy: number }>(
    undefined,
  );
  const zoomAt = (by: number, x: number, y: number) => {
    const box = scroller.current;
    if (!box) return;
    anchor.current = {
      x,
      y,
      cx: (box.scrollLeft + x) / scale,
      cy: (box.scrollTop + y) / scale,
    };
    setZoom((was) =>
      Math.min(ZOOM_MAX / fit, Math.max(ZOOM_MIN / fit, was * by)),
    );
  };
  useLayoutEffect(() => {
    const box = scroller.current;
    const at = anchor.current;
    if (!box || !at) return;
    box.scrollLeft = at.cx * scale - at.x;
    box.scrollTop = at.cy * scale - at.y;
    anchor.current = undefined;
  }, [scale]);
  // The middle of the view, for − and +.
  const middle = (by: number) =>
    scroller.current &&
    zoomAt(
      by,
      scroller.current.clientWidth / 2,
      scroller.current.clientHeight / 2,
    );
  // ⌘ or Ctrl and the wheel (a trackpad pinch) over the margin; over the
  // page, its helper sends the same as a zoom message.
  // biome-ignore lint/correctness/useExhaustiveDependencies: one listener, state by updater
  useEffect(() => {
    const box = scroller.current;
    if (!box) return;
    const wheel = (event: WheelEvent) => {
      if (!(event.ctrlKey || event.metaKey)) return;
      event.preventDefault();
      const at = box.getBoundingClientRect();
      zoomAt(
        Math.exp(-event.deltaY * 0.01),
        event.clientX - at.left,
        event.clientY - at.top,
      );
    };
    box.addEventListener("wheel", wheel, { passive: false });
    return () => box.removeEventListener("wheel", wheel);
  }, []);
  const select = (on: boolean) => {
    setSelecting(on);
    setHover(undefined);
    if (!on) setPicked(undefined);
    tell(frame.current, { type: "select", on });
  };
  // The design system the page links, by its tokens.css.
  const sheet = info?.sheets?.find((href) => /\/tokens\.css(\?|$)/.test(href));
  const system = sheet
    ? decodeURIComponent(new URL(sheet).pathname.slice(base.length))
    : "";
  const theme = useResource(
    () => (system ? read(system).then(readTokens) : undefined),
    [system],
  );
  const [themed, setThemed] = useState<Record<string, string>>({});
  const tryTheme = (name: string, value: string) => {
    setThemed((all) => ({ ...all, [name]: value }));
    tell(frame.current, { type: "tweak", name, value });
  };
  const declared = Object.entries(
    (info?.tweaks ?? {}) as Record<string, Tweak>,
  );
  const set = (name: string, value: unknown) => {
    setValues((all) => ({ ...all, [name]: value }));
    tell(frame.current, { type: "tweak", name, value });
  };
  const mark = (rect: Rect, kind: string) => (
    <span
      class={`page-mark ${kind}`}
      style={{
        left: `${rect.x * scale}px`,
        top: `${rect.y * scale}px`,
        width: `${rect.width * scale}px`,
        height: `${rect.height * scale}px`,
      }}
    />
  );
  return (
    <div class="open-page" data-tweaking={tweaking || undefined}>
      <div class="page-tools">
        <Button
          variant="secondary"
          aria-pressed={selecting}
          onClick={() => select(!selecting)}
        >
          <MousePointerClick />
          Select
        </Button>
        {(declared.length > 0 || !!theme.value?.colors.length) && (
          <Button
            variant="secondary"
            aria-pressed={tweaking}
            onClick={() => setTweaking(!tweaking)}
          >
            <SlidersHorizontal />
            Tweaks
          </Button>
        )}
        {document.fullscreenEnabled && (
          <IconButton
            label="Present"
            onClick={() => stage.current?.requestFullscreen()}
          >
            <Presentation />
          </IconButton>
        )}
        {picture && info && (
          <>
            <a
              class="file-button quiet icon-button"
              href={`${picture(size.w, size.h, 2)}&download=1`}
              download
              aria-label="Download as PNG"
              title="PNG"
            >
              <ImageDown />
            </a>
            <IconButton
              label="Copy as PNG"
              onClick={() =>
                navigator.clipboard.write([
                  new ClipboardItem({
                    "image/png": fetch(picture(size.w, size.h, 2)).then(
                      (response) => response.blob(),
                    ),
                  }),
                ])
              }
            >
              <Copy />
            </IconButton>
            <IconButton
              label="Draw on it"
              onClick={() =>
                view({ src: picture(size.w, size.h, 1), name: `${path}.png` })
              }
            >
              <PenLine />
            </IconButton>
          </>
        )}
        <a
          class="file-button quiet icon-button"
          href={`${src}#print`}
          target="_blank"
          rel="noopener"
          aria-label="Print or save as PDF"
          title="PDF"
        >
          <Printer />
        </a>
        <a
          class="file-button quiet icon-button"
          href={src}
          target="_blank"
          rel="noopener"
          aria-label="Open alone"
          title="Open alone"
        >
          <ExternalLink />
        </a>
      </div>
      <div class="page-body">
        <div
          class="page-stage"
          ref={stage}
          data-fixed={fixed || undefined}
          data-wide={(wide && !fixed) || undefined}
        >
          <div class="page-scroll" ref={scroller}>
            <div
              class="page-holder"
              style={
                wide
                  ? {
                      width: `${info!.artboard.w * scale}px`,
                      height: `${fixed ? info!.artboard.h * scale : room.h}px`,
                    }
                  : undefined
              }
            >
              <Frame
                key={load}
                src={src}
                title={info?.title || path}
                frameRef={frame}
                width={wide ? info!.artboard.w : undefined}
                height={
                  fixed ? info!.artboard.h : wide ? room.h / scale : undefined
                }
                scale={scale}
                onMessage={(message) => {
                  if (message.type === "ready") {
                    setSaid({ ...message, src });
                    if (selecting)
                      tell(frame.current, { type: "select", on: true });
                  } else if (message.type === "zoom") {
                    // A point of the page, where it sits in the scroller.
                    const page = frame.current!.getBoundingClientRect();
                    const box = scroller.current!.getBoundingClientRect();
                    zoomAt(
                      message.by,
                      page.left + message.x * scale - box.left,
                      page.top + message.y * scale - box.top,
                    );
                  } else if (message.type === "size")
                    // A page that grows (its fonts, its images) is pictured whole.
                    setSaid((was) => was && { ...was, height: message.height });
                  else if (message.type === "hover") setHover(message.rect);
                  else if (message.type === "pick") setPicked(message);
                  else if (message.type === "picked")
                    setPicked((was) => was && { ...was, rect: message.rect });
                  else if (
                    message.type === "key" &&
                    document.fullscreenElement
                  ) {
                    if (message.key === "ArrowRight") step(1);
                    if (message.key === "ArrowLeft") step(-1);
                  } else if (message.type === "key" && message.key === "Escape")
                    select(false);
                }}
              />
              {selecting && hover && mark(hover, "hover")}
              {picked && mark(picked.rect, "picked")}
            </div>
          </div>
          {wide && (
            <ZoomControls
              percent={Math.round(scale * 100)}
              out={() => middle(1 / ZOOM_STEP)}
              in={() => middle(ZOOM_STEP)}
              fit={zoom !== 1 ? () => setZoom(1) : undefined}
            />
          )}
        </div>
        {tweaking && (
          <aside class="page-tweaks" aria-label="Tweaks">
            {declared.map(([name, tweak]) => {
              const value = values[name] ?? tweak.default;
              const label = name.replace(/^--/, "");
              return (
                <div key={name} class="page-tweak">
                  <span class="small">
                    {label}
                    {tweak.editor !== "color" && tweak.editor !== "boolean" && (
                      <span class="muted">{` ${value ?? ""}${tweak.unit ?? ""}`}</span>
                    )}
                  </span>
                  {tweak.editor === "color" ? (
                    <div class="segmented">
                      {(tweak.options ?? []).map((option) => (
                        <Button
                          key={String(option)}
                          variant="secondary"
                          class="page-swatch"
                          aria-label={String(option)}
                          aria-pressed={value === option}
                          style={{ background: String(option) }}
                          onClick={() => set(name, option)}
                        />
                      ))}
                      <Input
                        type="color"
                        aria-label={`${label}, any colour`}
                        value={String(value ?? "")}
                        onInput={(event) =>
                          set(name, event.currentTarget.value)
                        }
                      />
                    </div>
                  ) : tweak.editor === "enum" ? (
                    <div class="segmented">
                      {(tweak.options ?? []).map((option) => (
                        <Button
                          key={String(option)}
                          variant="secondary"
                          aria-pressed={value === option}
                          onClick={() => set(name, option)}
                        >
                          {String(option)}
                        </Button>
                      ))}
                    </div>
                  ) : tweak.editor === "boolean" ? (
                    <Switch
                      label={label}
                      checked={!!value}
                      onChange={(checked) => set(name, checked)}
                    />
                  ) : (
                    <Input
                      type="range"
                      aria-label={label}
                      min={tweak.min ?? 0}
                      max={tweak.max ?? 100}
                      step={tweak.step ?? (tweak.editor === "int" ? 1 : 0.01)}
                      value={Number(value ?? 0)}
                      onInput={(event) =>
                        set(name, Number(event.currentTarget.value))
                      }
                    />
                  )}
                </div>
              );
            })}
            {theme.value && theme.value.colors.length > 0 && (
              <section class="page-theme" aria-label="Theme">
                <span class="small">
                  Theme <span class="muted">{system}</span>
                </span>
                {theme.value.themes.length > 1 && (
                  <div class="segmented">
                    {theme.value.themes.map((id) => (
                      <Button
                        key={id}
                        variant="secondary"
                        aria-pressed={
                          (themed.theme ?? theme.value!.themes[0]) === id
                        }
                        onClick={() => tryTheme("theme", id)}
                      >
                        {id}
                      </Button>
                    ))}
                  </div>
                )}
                {theme.value.colors
                  .filter((color) => /^#[0-9a-f]{6}$/i.test(color.value))
                  .map((color) => (
                    <label key={color.name} class="page-theme-token small">
                      <Input
                        type="color"
                        value={themed[`--${color.name}`] ?? color.value}
                        onInput={(event) =>
                          tryTheme(`--${color.name}`, event.currentTarget.value)
                        }
                      />
                      {color.name}
                    </label>
                  ))}
                {addToChat && Object.keys(themed).length > 0 && (
                  <Button
                    onClick={() => {
                      const { theme: id, ...colors } = themed;
                      addToChat(
                        `\`${system}\` theme ${id ?? theme.value!.themes[0]}: ${Object.entries(
                          colors,
                        )
                          .map(([name, value]) => `${name} ${value}`)
                          .join(", ")}`,
                      );
                    }}
                  >
                    Keep theme
                  </Button>
                )}
              </section>
            )}
            <div class="page-tweak-actions">
              <Button
                variant="secondary"
                onClick={() => {
                  setThemed({});
                  setValues({});
                  setLoad(load + 1);
                }}
              >
                Reset
              </Button>
              {addToChat && (
                <Button
                  disabled={!Object.keys(values).length}
                  onClick={() => addToChat(tweaksLine(path, values))}
                >
                  Keep
                </Button>
              )}
            </div>
          </aside>
        )}
      </div>
      {picked && addToChat && (
        <form
          class="page-comment"
          onSubmit={(event) => {
            event.preventDefault();
            if (!comment.trim()) return;
            addToChat(commentLine(path, picked, comment.trim()), true);
            setComment("");
            setPicked(undefined);
          }}
        >
          <span class="small muted">
            {`Comment on ${picked.tag}${picked.text ? ` “${picked.text}”` : ""}`}
          </span>
          <Textarea
            aria-label="Comment"
            rows={2}
            value={comment}
            onInput={(event) => setComment(event.currentTarget.value)}
          />
          <div class="page-tweak-actions">
            <Button
              variant="secondary"
              type="button"
              onClick={() => setPicked(undefined)}
            >
              Cancel
            </Button>
            <Button type="submit" disabled={!comment.trim()}>
              Add to chat
            </Button>
          </div>
        </form>
      )}
    </div>
  );
}
