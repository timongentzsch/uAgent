import type { PageInfo } from "./page-frame.tsx";

// A page is drawn this wide when it names no size.
const PAGE_WIDTH = 1440;
// How tall a page's frame may grow on the canvas.
const PAGE_HEIGHT_MAX = 4000;

// The size a page is drawn at: its artboard, or as wide as it says (else
// PAGE_WIDTH) and as tall as it measured, up to `most`.
export function pageSize(info?: PageInfo, most = PAGE_HEIGHT_MAX) {
  const fixed = !!info?.artboard.w && !!info.artboard.h;
  return {
    fixed,
    w: info?.artboard.w || PAGE_WIDTH,
    h: fixed ? info!.artboard.h : Math.min(most, info?.height || 900),
  };
}
