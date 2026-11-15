import { useCallback, useEffect, useState } from "preact/hooks";
import type { InstallPrompt, Report } from "../shared/types.ts";
import { stored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { prefetchMarkdown } from "../shared/markdown-view.tsx";
import { focusDecision, followDecisionLink } from "../shared/navigation.ts";
import { trackViewport } from "../shared/layout.ts";
import { useLatest } from "../shared/use-latest.ts";
import { useResource } from "../shared/use-resource.ts";
import { useApi } from "../state/api.ts";

// Kept per device, so the header's browser slot is right from the first frame.
const browserKept = stored(storageKeys.browser, false, (value) => value === 1, {
  kept: Number,
});

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
  const { api, routes } = useApi();
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
    // Not on idle: on mobile that can fire after the first snapshot mounted,
    // and the plain-to-markdown height change then fights the bottom pin.
    prefetchMarkdown();
  }, []);
  useEffect(trackViewport, []);
  // Unhandled, a dropped file would open in place of the app.
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
