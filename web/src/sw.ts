declare const self: ServiceWorkerGlobalScope & {
  __WB_MANIFEST: import("workbox-precaching").PrecacheEntry[];
};
interface Attention {
  id?: string;
  session_id?: string;
}
import {
  precacheAndRoute,
  createHandlerBoundToURL,
  matchPrecache,
} from "workbox-precaching";
import { registerRoute, NavigationRoute } from "workbox-routing";
// Exact build-manifest URLs only. API, SSE, uploads, credentials and drafts
// never enter a cache; mutations never enter an offline queue.
precacheAndRoute(self.__WB_MANIFEST);
registerRoute(
  new NavigationRoute(createHandlerBoundToURL("/index.html"), {
    allowlist: [/^\/$/],
  }),
);
const RENDERERS = "uagent-renderers-";
let rendererConfig: Promise<{ assets: Set<string>; cache: string }> | undefined;
function renderers() {
  return (rendererConfig ??= matchPrecache("/renderer-assets.json")
    .then((response) => response?.json())
    .then((paths: unknown) => {
      const list = Array.isArray(paths)
        ? paths.filter((path): path is string => typeof path === "string")
        : [];
      let revision = 2166136261;
      for (const character of list.join("\n"))
        revision = Math.imul(revision ^ character.charCodeAt(0), 16777619);
      return {
        assets: new Set(list),
        cache: `${RENDERERS}${(revision >>> 0).toString(16)}`,
      };
    })
    .catch(() => ({
      assets: new Set<string>(),
      cache: `${RENDERERS}empty`,
    })));
}
async function cacheRenderers(paths: unknown) {
  const { assets, cache: cacheName } = await renderers();
  const cache = await caches.open(cacheName);
  if (!Array.isArray(paths)) return;
  await Promise.all(
    paths.map(async (path) => {
      if (typeof path !== "string") return;
      const url = new URL(path, self.location.origin);
      if (url.origin !== self.location.origin || !assets.has(url.pathname))
        return;
      const request = new Request(url.href, { credentials: "same-origin" });
      if (await cache.match(request)) return;
      const response = await fetch(request);
      if (response.ok) await cache.put(request, response);
    }),
  );
}
registerRoute(
  ({ url, request }) =>
    request.method === "GET" &&
    url.origin === self.location.origin &&
    url.pathname.startsWith("/assets/"),
  async ({ url, request }) => {
    const { assets, cache: cacheName } = await renderers();
    if (!assets.has(url.pathname)) return fetch(request);
    const cache = await caches.open(cacheName);
    const cached = await cache.match(request);
    if (cached) return cached;
    const response = await fetch(request);
    if (response.ok) await cache.put(request, response.clone());
    return response;
  },
);
self.addEventListener("activate", (event) => {
  event.waitUntil(
    renderers().then(async ({ cache }) => {
      for (const name of await caches.keys())
        if (name.startsWith(RENDERERS) && name !== cache)
          await caches.delete(name);
    }),
  );
});
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
  if (event.data?.type === "CACHE_RENDERERS")
    event.waitUntil(cacheRenderers(event.data.paths));
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
        existing.postMessage({ type: "OPEN_SESSION", id });
        await existing.focus();
      } else await self.clients.openWindow(target);
    })(),
  );
});
