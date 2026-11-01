import { coarsePointer } from "./layout.ts";

// A download link's click: on a touch device that can share files the file
// goes to the system share sheet; anywhere else the link downloads as usual.
// If the share is refused (the fetch outlasted the tap's permission), the
// file downloads. A cancelled sheet ends quietly.
export function shareOrDownload(event: Event) {
  const { href, download } = event.currentTarget as HTMLAnchorElement;
  if (
    !coarsePointer() ||
    !navigator.canShare?.({ files: [new File([], download)] })
  )
    return;
  event.preventDefault();
  void (async () => {
    try {
      const blob = await (await fetch(href)).blob();
      await navigator.share({
        files: [new File([blob], download, { type: blob.type })],
      });
    } catch (error) {
      if ((error as Error).name !== "AbortError") location.href = href;
    }
  })();
}
