import { useCallback, useEffect, useState } from "preact/hooks";
import type { InstallPrompt, Report } from "../shared/types.ts";
import { stored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { idlePreloadMs } from "../shared/limits.ts";
import { preloadDeferred } from "../shared/ui.tsx";
// Prefetch helpers live next to the renderer so marker regexes stay in one
// place.
import { prefetchMarkdown } from "../shared/markdown-view.tsx";
import { focusDecision, followDecisionLink } from "../shared/navigation.ts";
import { trackViewport } from "../shared/layout.ts";
import { useLatest } from "../shared/use-latest.ts";
import { useResource } from "../shared/use-resource.ts";
import { api, routes } from "../state/api.ts";
import * as dialogs from "./dialogs.ts";

// Remembered per device, so the header's browser slot is right from the
// first frame instead of appearing once the host answers.
const browserKept = stored(storageKeys.browser, false, (value) => value === 1, {
  kept: Number,
});

// What the shell does once it starts, apart from any conversation: warms
// what it will draw, follows the viewport, takes files dropped on the page,
// hears from the service worker, and learns whether the host has a browser.
export function useBoot({
  upload,
  page,
  choose,
  report,
  authenticated,
  online,
}: {
  upload: (files: File[]) => Promise<boolean>;
  page: string;
  choose: (id: string) => Promise<void>;
  report: Report;
  authenticated: boolean | null;
  online: boolean;
}) {
  const browserAvailable = !!useResource(
    (signal) =>
      authenticated === true && online
        ? api(routes.browserStatus, undefined, { signal }).then(
            () => true,
            () => false,
          )
        : undefined,
    [authenticated, online],
    browserKept.read,
  ).value;
  useEffect(() => browserKept.write(browserAvailable), [browserAvailable]);
  const [install, setInstall] = useState<InstallPrompt | null>(null);
  const [update, setUpdate] = useState<ServiceWorker | null>(null);
  useEffect(() => {
    // The core renderer chunk is needed for every assistant message, so
    // fetch it immediately at boot (not idle: on mobile the idle callback
    // can fire after the first snapshot already mounted, which spreads
    // the plain-to-markdown height wave across the first seconds and
    // fights the bottom pin). Marker-based chunks warm per text below.
    prefetchMarkdown();
    // Every dialog and page a click opens, parsed once the shell has
    // settled (the service worker already holds it), so a first open
    // animates its content, not a placeholder for its code.
    const warm = setTimeout(
      () =>
        Object.values(dialogs).forEach((load) =>
          preloadDeferred<never>(load).catch(() => {}),
        ),
      idlePreloadMs,
    );
    return () => clearTimeout(warm);
  }, []);
  useEffect(trackViewport, []);
  // Files dropped anywhere attach to the open conversation; unhandled, the
  // browser would open the file in place of the app. File inputs and
  // dialogs keep their own drops.
  const drops = useLatest({ upload, page });
  const attach = useCallback(
    (files: File[]) => drops.current.upload(files),
    [],
  );
  useEffect(() => {
    const files = (event: DragEvent) =>
      event.dataTransfer?.types.includes("Files") &&
      !(
        event.target instanceof HTMLInputElement && event.target.type === "file"
      );
    const over = (event: DragEvent) => files(event) && event.preventDefault();
    const drop = (event: DragEvent) => {
      if (!files(event)) return;
      event.preventDefault();
      if (
        drops.current.page === "chat" &&
        !(event.target as Element).closest?.("dialog")
      )
        void drops.current.upload([...(event.dataTransfer?.files || [])]);
    };
    addEventListener("dragover", over);
    addEventListener("drop", drop);
    return () => {
      removeEventListener("dragover", over);
      removeEventListener("drop", drop);
    };
  }, []);
  useEffect(() => {
    let stop: (() => void) | undefined;
    import("../shared/pwa.ts").then(
      ({ watchPwa }) => (stop = watchPwa(setInstall, setUpdate, report)),
    );
    return () => stop?.();
  }, [report]);
  useEffect(() => {
    const openSession = (event: MessageEvent) => {
      if (
        event.data?.type === "OPEN_SESSION" &&
        /^[a-f0-9]{16,64}$/.test(event.data.id || "")
      ) {
        choose(event.data.id);
        if (event.data.decision !== undefined) focusDecision();
      }
    };
    // A notification's link opened this window at a decision.
    followDecisionLink();
    addEventListener("hashchange", followDecisionLink);
    navigator.serviceWorker?.addEventListener("message", openSession);
    return () => {
      navigator.serviceWorker?.removeEventListener("message", openSession);
      removeEventListener("hashchange", followDecisionLink);
    };
  }, [choose]);
  return { browserAvailable, install, setInstall, update, attach };
}
