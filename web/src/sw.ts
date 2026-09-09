declare const self: ServiceWorkerGlobalScope & {
  __WB_MANIFEST: import("workbox-precaching").PrecacheEntry[];
};
interface Attention {
  id?: string;
  session_id?: string;
}
import { precacheAndRoute, createHandlerBoundToURL } from "workbox-precaching";
import { registerRoute, NavigationRoute } from "workbox-routing";
// Exact build-manifest URLs only. API, SSE, uploads, credentials and drafts
// never enter a cache; mutations never enter an offline queue.
precacheAndRoute(self.__WB_MANIFEST);
registerRoute(
  new NavigationRoute(createHandlerBoundToURL("/index.html"), {
    allowlist: [/^\/$/],
  }),
);
const shown = new Set<string>();
async function attention(data: Attention | undefined) {
  if (
    !data ||
    !/^[a-f0-9:.-]{1,150}$/.test(data.id || "") ||
    !/^[a-f0-9]{16,64}$/.test(data.session_id || "")
  )
    return;
  await self.registration.showNotification("µAgent needs your attention", {
    body: "Open the app to review the latest session state.",
    tag: data.id,
    icon: "/icon-192.png",
    badge: "/icon-192.png",
    data: { session_id: data.session_id },
  });
}
self.addEventListener("message", (event) => {
  if (event.data?.type === "ACTIVATE_UPDATE") self.skipWaiting();
  if (event.data?.type === "ATTENTION" && !shown.has(event.data.id)) {
    shown.add(event.data.id);
    if (shown.size > 256) shown.delete(shown.values().next().value!);
    event.waitUntil(attention(event.data));
  }
});
self.addEventListener("push", (event) => {
  // Every delivered push attempts a user-visible notification. No silent push,
  // sensitive fetch, or approval/execution from a background event.
  let data;
  try {
    data = event.data?.json();
  } catch {
    return;
  }
  event.waitUntil(attention(data));
});
self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  const id = event.notification.data?.session_id;
  if (!/^[a-f0-9]{16,64}$/.test(id || "")) return;
  const target = new URL(`/#session=${id}`, self.location.origin).href;
  event.waitUntil(
    (async () => {
      const windows = await self.clients.matchAll({
        type: "window",
        includeUncontrolled: true,
      });
      const existing = windows.find(
        (window) => new URL(window.url).origin === self.location.origin,
      );
      if (existing) {
        await existing.navigate(target);
        await existing.focus();
      } else await self.clients.openWindow(target);
    })(),
  );
});
