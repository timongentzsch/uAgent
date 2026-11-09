import { coarsePointer } from "./layout.ts";
import { downloadRevokeMs } from "./limits.ts";

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

// An address downloaded as a file named `name`.
export function saveLink(href: string, name: string) {
  const link = document.createElement("a");
  link.href = href;
  link.download = name;
  link.click();
}

// Bytes made in the page, downloaded as a file named `name`.
export function downloadBlob(blob: Blob, name: string) {
  const url = URL.createObjectURL(blob);
  saveLink(url, name);
  // Some browsers start the download after click() returns.
  setTimeout(() => URL.revokeObjectURL(url), downloadRevokeMs);
}
