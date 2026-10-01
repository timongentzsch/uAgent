import { api } from "./api.ts";

// Background push for this device: the browser's subscription, and the host
// told about it. `key` is the host's VAPID public key, base64url.
export async function subscribePush(key: string) {
  const registration = await navigator.serviceWorker.ready;
  const subscription =
    (await registration.pushManager.getSubscription()) ||
    (await registration.pushManager.subscribe({
      userVisibleOnly: true,
      applicationServerKey: Uint8Array.from(
        atob(key.replace(/-/g, "+").replace(/_/g, "/")),
        (value) => value.charCodeAt(0),
      ),
    }));
  await api("/api/push/subscriptions", subscription.toJSON());
}

export async function unsubscribePush() {
  await api("/api/push/subscriptions", undefined, { method: "DELETE" });
  const registration = await navigator.serviceWorker.getRegistration();
  await (await registration?.pushManager?.getSubscription())?.unsubscribe();
}
