import { useEffect, useRef } from "preact/hooks";
import { motionMs } from "./motion.ts";

// Each pixel's threshold: an ordered 4×4 dither.
const BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
const ROWS = 3;
// The track the glow runs on: the pixels with the lowest thresholds.
const TRACK = 0.1;

// Draws the band with the glow's head at `head` (a column), heading `way`.
function draw(canvas: HTMLCanvasElement, head: number, way: number) {
  const cols = canvas.width;
  const ink = canvas.getContext("2d");
  if (!ink) return;
  ink.clearRect(0, 0, cols, ROWS);
  ink.fillStyle = getComputedStyle(canvas).color;
  // A longer tail behind the head than glow ahead of it.
  const reach = cols / 3.2;
  for (let x = 0; x < cols; x++) {
    const off = (head - x) * way;
    const glow = Math.exp(-((off / (off > 0 ? reach : reach / 3)) ** 2));
    for (let y = 0; y < ROWS; y++)
      if (Math.max(TRACK, glow) > (BAYER[(y % 4) * 4 + (x % 4)] + 0.5) / 16)
        ink.fillRect(x, y, 1, 1);
  }
}

// The status line's sign that work is under way: a dithered glow runs to
// and fro along a band of pixels, one pass in --motion-work. It steps only
// when its head moves a pixel, and stops while the page is hidden; with
// motion off it stands still, mid-pass.
export function WorkBand() {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    canvas.height = ROWS;
    canvas.width = Math.max(
      2,
      Math.round(canvas.clientWidth / (canvas.clientHeight / ROWS)),
    );
    const last = canvas.width - 1;
    const start = performance.now();
    let timer: ReturnType<typeof setTimeout> | undefined;
    const step = () => {
      const pass = motionMs("work");
      if (!pass) return draw(canvas, last / 2, 1);
      const phase = ((performance.now() - start) / pass) % 2;
      const [at, way] = phase < 1 ? [phase, 1] : [2 - phase, -1];
      draw(canvas, Math.round(at * last), way);
      if (document.visibilityState === "visible")
        timer = setTimeout(step, pass / last);
    };
    const shown = () => {
      clearTimeout(timer);
      step();
    };
    step();
    document.addEventListener("visibilitychange", shown);
    return () => {
      clearTimeout(timer);
      document.removeEventListener("visibilitychange", shown);
    };
  }, []);
  return <canvas ref={ref} class="work-band" aria-hidden="true" />;
}
