// Markup on an image to steer the agent: freehand strokes and numbered pins
// with a note each, baked into a copy that is attached like any upload.
// One Pointer Events path serves mouse, trackpad, finger and pencil.
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { getStroke } from "perfect-freehand";
import { MapPin, PenLine, Undo2 } from "lucide-preact";
import { Button, DialogHeader, IconButton, LoadError, Spinner } from "./ui.tsx";
import { Input } from "./form-controls.tsx";
import { capturePointer, TAP_SLOP_PX, type ZoomView } from "./zoom.ts";
import { ZoomSurface } from "./zoom-surface.tsx";
import { maxUploadBytes } from "./limits.ts";
import "./annotate.css";

type Point = [x: number, y: number, pressure: number];
type Stroke = { kind: "stroke"; points: Point[]; pen: boolean };
type Pin = { kind: "pin"; x: number; y: number; note: string };
type Item = Stroke | Pin;

// Mark sizes follow the image, so they read the same at any resolution.
function metrics(width: number, height: number) {
  const base = Math.hypot(width, height);
  const radius = Math.max(10, base / 70);
  return { stroke: Math.max(3, base / 350), radius, font: radius * 1.1 };
}

// perfect-freehand's outline as a closed path of quadratic segments.
function outline(stroke: Stroke, size: number, last: boolean) {
  const points = getStroke(stroke.points, {
    size,
    thinning: 0.5,
    simulatePressure: !stroke.pen,
    last,
  });
  if (!points.length) return new Path2D();
  const d = points.reduce(
    (path, [x0, y0], index, all) => {
      const [x1, y1] = all[(index + 1) % all.length];
      path.push(x0, y0, (x0 + x1) / 2, (y0 + y1) / 2);
      return path;
    },
    ["M", ...points[0], "Q"] as (string | number)[],
  );
  return new Path2D(`${d.join(" ")} Z`);
}

// A pin's number: its place among the pins, strokes not counted.
const pinNumber = (items: Item[], index: number) =>
  items.slice(0, index + 1).filter((item) => item.kind === "pin").length;

// The mark colour and the colour of text on it, from the theme tokens.
function colors() {
  const style = getComputedStyle(document.documentElement);
  return {
    mark: style.getPropertyValue("--mark"),
    onMark: style.getPropertyValue("--on-mark"),
  };
}

// Wrapped lines of `text` no wider than `width`.
function wrap(ctx: CanvasRenderingContext2D, text: string, width: number) {
  const lines: string[] = [];
  let line = "";
  for (const word of text.split(/\s+/).filter(Boolean)) {
    const next = line ? `${line} ${word}` : word;
    if (line && ctx.measureText(next).width > width) {
      lines.push(line);
      line = word;
    } else line = next;
  }
  if (line) lines.push(line);
  return lines;
}

// Everything drawn in image coordinates: the screen canvas scales the
// context, the export draws at natural size. `skip` is the pin whose note
// is being edited in its bubble.
function paint(
  ctx: CanvasRenderingContext2D,
  image: ImageBitmap,
  items: Item[],
  live: Stroke | null,
  skip: number | null,
  { mark, onMark }: ReturnType<typeof colors>,
) {
  const { width, height } = image;
  const size = metrics(width, height);
  ctx.drawImage(image, 0, 0);
  ctx.fillStyle = mark;
  for (const item of items)
    if (item.kind === "stroke") ctx.fill(outline(item, size.stroke, true));
  if (live) ctx.fill(outline(live, size.stroke, false));
  ctx.font = `600 ${size.font}px ${getComputedStyle(document.body).fontFamily}`;
  ctx.textBaseline = "middle";
  items.forEach((item, index) => {
    if (item.kind !== "pin") return;
    const { x, y } = item;
    const r = size.radius;
    ctx.fillStyle = mark;
    ctx.beginPath();
    ctx.arc(x, y, r, 0, Math.PI * 2);
    ctx.fill();
    ctx.lineWidth = r / 6;
    ctx.strokeStyle = onMark;
    ctx.stroke();
    ctx.fillStyle = onMark;
    ctx.textAlign = "center";
    ctx.fillText(String(pinNumber(items, index)), x, y);
    if (index === skip || !item.note.trim()) return;
    // The note sits beside its pin, flipped and clamped to stay inside.
    ctx.textAlign = "left";
    const pad = r / 2;
    const lineHeight = size.font * 1.3;
    const lines = wrap(ctx, item.note.trim(), Math.max(width * 0.4, r * 8));
    const boxW =
      Math.max(...lines.map((line) => ctx.measureText(line).width)) + pad * 2;
    const boxH = lines.length * lineHeight + pad;
    let left = x + r * 1.4;
    if (left + boxW > width) left = x - r * 1.4 - boxW;
    left = Math.max(0, Math.min(left, width - boxW));
    const top = Math.max(0, Math.min(y - boxH / 2, height - boxH));
    ctx.fillStyle = mark;
    ctx.beginPath();
    ctx.roundRect(left, top, boxW, boxH, pad);
    ctx.fill();
    ctx.fillStyle = onMark;
    lines.forEach((line, row) =>
      ctx.fillText(line, left + pad, top + pad / 2 + lineHeight * (row + 0.5)),
    );
  });
}

export default function Annotator({
  src,
  name,
  cancel,
  attach,
}: {
  src: string;
  name: string;
  cancel: () => void;
  attach: (file: File) => Promise<void>;
}) {
  const stage = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [image, setImage] = useState<ImageBitmap | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  const [items, setItems] = useState<Item[]>([]);
  const [tool, setTool] = useState<"pen" | "pin">("pen");
  const [editing, setEditing] = useState<number | null>(null);
  // The fit: image pixels to stage pixels at zoom 1, and the stage's size.
  const [scale, setScale] = useState(0);
  const [stageSize, setStageSize] = useState({ width: 0, height: 0 });
  // The zoom over that fit (see ZoomSurface).
  const [view, setView] = useState<ZoomView>({ scale: 1, x: 0, y: 0 });
  const [busy, setBusy] = useState(false);
  // A failed export keeps the markup on screen, unlike a failed load.
  const [saveError, setSaveError] = useState<unknown>(null);
  const live = useRef<Stroke | null>(null);
  const press = useRef<{ id: number; x: number; y: number } | null>(null);

  useEffect(() => {
    let active = true;
    let loaded: ImageBitmap | undefined;
    setError(null);
    fetch(src)
      .then((response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.blob();
      })
      .then(createImageBitmap)
      .then((bitmap) => {
        loaded = bitmap;
        if (active) setImage(bitmap);
        else bitmap.close();
      })
      .catch((failure) => active && setError(failure));
    return () => {
      active = false;
      // The decoded image holds its full-size pixels until closed.
      loaded?.close();
    };
  }, [src, attempt]);

  // Fit the image to the stage; the backing store follows the pixel ratio.
  useLayoutEffect(() => {
    const element = stage.current;
    if (!image || !element) return;
    const fit = () => {
      const { width, height } = element.getBoundingClientRect();
      setStageSize({ width, height });
      setScale(Math.min(width / image.width, height / image.height));
    };
    fit();
    const observer = new ResizeObserver(fit);
    observer.observe(element);
    return () => observer.disconnect();
  }, [image]);

  const draw = () => {
    const element = canvas.current;
    if (!image || !element || !scale) return;
    // Sharp at the current zoom, but never finer than twice the image.
    const ratio = Math.min(devicePixelRatio * view.scale, 2 / scale);
    const width = Math.round(image.width * scale * ratio);
    const height = Math.round(image.height * scale * ratio);
    // Resizing clears and reallocates; a stroke in progress only repaints.
    if (element.width !== width) element.width = width;
    if (element.height !== height) element.height = height;
    const ctx = element.getContext("2d")!;
    ctx.setTransform(scale * ratio, 0, 0, scale * ratio, 0, 0);
    paint(ctx, image, items, live.current, editing, colors());
  };
  useLayoutEffect(draw, [image, items, editing, scale, view.scale]);

  const undo = () => {
    if (editing === items.length - 1) setEditing(null);
    setItems(items.slice(0, -1));
  };
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.target instanceof HTMLInputElement) return;
      if ((event.metaKey || event.ctrlKey) && event.key === "z") {
        event.preventDefault();
        undo();
      }
    };
    addEventListener("keydown", onKey);
    return () => removeEventListener("keydown", onKey);
  }, [items, editing]);

  // Screen pixels per image pixel, with the zoom.
  const pixel = () => scale * view.scale;
  const at = (event: PointerEvent): Point => {
    const rect = canvas.current!.getBoundingClientRect();
    return [
      (event.clientX - rect.left) / pixel(),
      (event.clientY - rect.top) / pixel(),
      event.pressure || 0.5,
    ];
  };

  const down = (event: PointerEvent) => {
    // A second finger (palm, pinch) abandons the stroke in progress.
    if (!event.isPrimary) {
      live.current = null;
      press.current = null;
      draw();
      return;
    }
    if (event.pointerType === "mouse" && event.button !== 0) return;
    capturePointer(canvas.current!, event.pointerId);
    press.current = { id: event.pointerId, x: event.clientX, y: event.clientY };
    if (tool === "pen")
      live.current = {
        kind: "stroke",
        points: [at(event)],
        pen: event.pointerType === "pen",
      };
  };
  const move = (event: PointerEvent) => {
    if (press.current?.id !== event.pointerId || !live.current) return;
    for (const sample of event.getCoalescedEvents?.() || [event])
      live.current.points.push(at(sample));
    draw();
  };
  const up = (event: PointerEvent) => {
    const start = press.current;
    if (start?.id !== event.pointerId) return;
    press.current = null;
    const stroke = live.current;
    live.current = null;
    if (stroke) {
      setItems([...items, stroke]);
      return;
    }
    if (
      Math.hypot(event.clientX - start.x, event.clientY - start.y) > TAP_SLOP_PX
    )
      return;
    const [x, y] = at(event);
    // A finger-sized target even where the badge is drawn small.
    const radius = Math.max(
      metrics(image!.width, image!.height).radius,
      22 / pixel(),
    );
    const hit = items.findIndex(
      (item) =>
        item.kind === "pin" && Math.hypot(item.x - x, item.y - y) <= radius,
    );
    if (hit >= 0) return setEditing(hit);
    setItems([...items, { kind: "pin", x, y, note: "" }]);
    setEditing(items.length);
  };
  const lift = () => {
    press.current = null;
    live.current = null;
    draw();
  };

  const note = (value: string) =>
    setItems(
      items.map((item, index) =>
        index === editing && item.kind === "pin"
          ? { ...item, note: value }
          : item,
      ),
    );

  async function done() {
    if (!image) return;
    setBusy(true);
    setSaveError(null);
    try {
      const output = document.createElement("canvas");
      output.width = image.width;
      output.height = image.height;
      paint(output.getContext("2d")!, image, items, null, null, colors());
      const encode = (type: string) =>
        new Promise<Blob | null>((resolve) =>
          output.toBlob(resolve, type, 0.9),
        );
      let blob = await encode("image/png");
      let extension = "png";
      if (!blob || blob.size > maxUploadBytes) {
        blob = await encode("image/jpeg");
        extension = "jpg";
      }
      if (!blob) throw new Error("Could not encode the annotated image.");
      const base = name.replace(/\.[^.]+$/, "") || "image";
      await attach(
        new File([blob], `${base}-annotated.${extension}`, { type: blob.type }),
      );
    } catch (failure) {
      setSaveError(failure);
    } finally {
      setBusy(false);
    }
  }

  const pin = editing === null ? null : items[editing];
  // Where the image sits in the stage: centred at the fit, then zoomed.
  const origin = image && {
    x: view.x + (view.scale * (stageSize.width - image.width * scale)) / 2,
    y: view.y + (view.scale * (stageSize.height - image.height * scale)) / 2,
  };
  return (
    <>
      <DialogHeader
        title={name}
        actions={
          <>
            <Button variant="quiet" onClick={cancel}>
              Cancel
            </Button>
            <Button
              variant="primary"
              busy={busy}
              disabled={!image || !items.length}
              onClick={done}
            >
              Attach
            </Button>
          </>
        }
      />
      <div class="dialog-body annotator" data-tool={tool}>
        {error ? (
          <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
        ) : !image ? (
          <Spinner surface />
        ) : (
          <>
            <div
              class="annotator-stage"
              ref={stage}
              style={{
                "--w": `${image.width * scale}px`,
                "--h": `${image.height * scale}px`,
              }}
            >
              <ZoomSurface
                label={name}
                draw
                natural={image.width}
                onView={setView}
              >
                <canvas
                  ref={canvas}
                  aria-label={`Annotate ${name}`}
                  onPointerDown={down}
                  onPointerMove={move}
                  onPointerUp={up}
                  onPointerCancel={lift}
                />
              </ZoomSurface>
              {pin?.kind === "pin" && origin && (
                <div
                  class="annotator-note"
                  // Opens away from the nearer edge, like the baked label.
                  data-side={pin.x > image.width / 2 ? "left" : "right"}
                  style={{
                    "--x": `${origin.x + pin.x * pixel()}px`,
                    "--y": `${origin.y + pin.y * pixel()}px`,
                    "--r": `${metrics(image.width, image.height).radius * pixel()}px`,
                  }}
                >
                  <Input
                    aria-label={`Note for pin ${pinNumber(items, editing!)}`}
                    placeholder="What should change here?"
                    value={pin.note}
                    autoFocus
                    enterKeyHint="done"
                    onInput={(event) => note(event.currentTarget.value)}
                    onBlur={() => setEditing(null)}
                    onKeyDown={(event) => {
                      if (event.key !== "Enter" && event.key !== "Escape")
                        return;
                      // Escape closes the bubble, not the viewer.
                      event.preventDefault();
                      event.stopPropagation();
                      setEditing(null);
                    }}
                  />
                </div>
              )}
            </div>
            {saveError && <LoadError error={saveError} />}
            <div class="annotator-tools">
              <div class="segmented" aria-label="Annotation tool">
                <Button
                  size="compact"

                  aria-pressed={tool === "pen"}
                  onClick={() => setTool("pen")}
                >
                  <PenLine />
                  Pen
                </Button>
                <Button
                  size="compact"

                  aria-pressed={tool === "pin"}
                  onClick={() => setTool("pin")}
                >
                  <MapPin />
                  Pin
                </Button>
              </div>
              <IconButton label="Undo" disabled={!items.length} onClick={undo}>
                <Undo2 />
              </IconButton>
            </div>
          </>
        )}
      </div>
    </>
  );
}
