import "./settings.css";
import type { Dispatch, StateUpdater, MutableRef } from "preact/hooks";
import type {
  Sizes,
  InstallPrompt,
  Draft,
  Snapshot,
  Catalogue,
  Session,
} from "./types.ts";
import { Deferred, Field, Select, Skeleton, LoadError } from "./ui.tsx";
import { useEffect, useRef, useState } from "preact/hooks";
const configuration = () => import("./configuration.tsx");
import { api, command } from "./store.ts";
export default function Settings({
  theme,
  setTheme,
  sizes,
  setSizes,
  installed,
  install,
  setInstall,
  ios,
  update,
  drafts,
  uploading,
  snapshots,
  catalogue,
  online,
  notificationMode,
  setNotificationMode,
  notifications,
  refresh,
  selected,
  session,
  logout,
}: {
  theme: string;
  setTheme: Dispatch<StateUpdater<string>>;
  sizes: Sizes;
  setSizes: Dispatch<StateUpdater<Sizes>>;
  installed: boolean;
  install: InstallPrompt | null;
  setInstall: Dispatch<StateUpdater<InstallPrompt | null>>;
  ios: boolean;
  update: ServiceWorker | null;
  drafts: Record<string, Draft>;
  uploading: boolean;
  snapshots: Record<string, Snapshot>;
  catalogue: Catalogue;
  online: boolean;
  notificationMode: boolean;
  setNotificationMode: Dispatch<StateUpdater<boolean>>;
  notifications: MutableRef<boolean>;
  refresh: () => Promise<void>;
  selected: string;
  session?: Session;
  logout: () => Promise<void>;
}) {
  const [advanced, setAdvanced] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const report = setError;
  const [attempt, setAttempt] = useState(0);
  const body = useRef<HTMLDivElement>(null);
  useEffect(() => {
    body.current?.closest(".dialog-body")?.scrollTo(0, 0);
  }, [advanced]);
  const [permission, setPermission] = useState<string | null>(null);
  const [savingPermission, setSavingPermission] = useState(false);
  useEffect(() => {
    let active = true;
    setError(null);
    command("config", null, { name: "UAGENT_APPROVAL" })
      .then((response) => {
        if (!active) return;
        if (response.pending)
          throw new Error("Permissions are still loading. Try again shortly.");
        setPermission(
          response.result.settings[0]?.value === "yolo" ? "yolo" : "prompt",
        );
      })
      .catch((failure) => {
        if (active) setError(failure);
      });
    return () => {
      active = false;
    };
  }, [attempt]);
  return (
    <div ref={body} class="settings-content">
      {advanced ? (
        <>
          <div class="subview-head">
            <button type="button" onClick={() => setAdvanced(false)}>
              ← Back
            </button>
            <h3>Advanced configuration</h3>
          </div>
          <Deferred
            load={configuration}
            session={session}
            online={online}
            fallback={<Skeleton className="form-skeleton" rows={5} />}
          />
        </>
      ) : (
        <>
          <div class="settings-fields">
            <Field label="Appearance">
              <Select
                aria-label="Appearance"
                value={theme}
                onChange={(event) => setTheme(event.currentTarget.value)}
              >
                <option value="system">System</option>
                <option value="dark">Dark</option>
                <option value="light">Light</option>
              </Select>
            </Field>
            {(
              [
                ["Display size", "display"],
                ["Text size", "text"],
              ] as const
            ).map(([label, key]) => (
              <Field key={key} label={label} value={`${sizes[key]}%`}>
                <input
                  type="range"
                  aria-label={label}
                  min="50"
                  max={key === "text" ? 300 : 200}
                  step="1"
                  value={sizes[key]}
                  onInput={(event) =>
                    setSizes({
                      ...sizes,
                      [key]: Number(event.currentTarget.value),
                    })
                  }
                />
              </Field>
            ))}
            <div class="dialog-actions">
              <button
                type="button"
                onClick={() => setSizes({ display: 100, text: 100 })}
              >
                Reset sizes
              </button>
            </div>
            {permission === null ? (
              error ? (
                <LoadError
                  error={error}
                  retry={() => setAttempt(attempt + 1)}
                />
              ) : (
                <Field
                  label="Default permissions"
                  help="Used by new conversations and conversations that inherit the default."
                >
                  <Skeleton
                    rows={1}
                    className="control-skeleton"
                    label="Loading default permissions…"
                  />
                </Field>
              )
            ) : (
              <Field
                label="Default permissions"
                help="Used by new conversations and conversations that inherit the default."
              >
                <Select
                  aria-label="Default permissions"
                  value={permission}
                  disabled={!online || savingPermission}
                  onChange={async (event) => {
                    const value = event.currentTarget.value;
                    setSavingPermission(true);
                    setError(null);
                    try {
                      const result = await command("config", null, {
                        operation: "apply",
                        scope: "user",
                        changes: [{ key: "UAGENT_APPROVAL", value }],
                      });
                      if (result.pending)
                        throw new Error(
                          "The change is still pending. Reopen settings to check its status.",
                        );
                      setPermission(value);
                    } catch (failure) {
                      setError(failure);
                    } finally {
                      setSavingPermission(false);
                    }
                  }}
                >
                  <option value="prompt">Ask</option>
                  <option value="yolo">
                    YOLO · automatic ordinary approvals
                  </option>
                </Select>
              </Field>
            )}
            {permission !== null && error && <LoadError error={error} />}
            <button type="button" onClick={() => setAdvanced(true)}>
              Advanced configuration
            </button>
          </div>
          <details class="settings-section">
            <summary>Install</summary>
            {installed ? (
              <p>Running as an installed app.</p>
            ) : install ? (
              <button
                onClick={async () => {
                  try {
                    await install.prompt();
                  } catch (error) {
                    report(error);
                  }
                  setInstall(null);
                }}
              >
                Install µAgent
              </button>
            ) : (
              <p>
                {ios
                  ? "In Safari, choose Share → Add to Home Screen. Open the installed app and pair it if needed."
                  : "Use your browser’s install option where supported. Installation needs a trusted HTTPS origin or localhost."}
              </p>
            )}
          </details>
          {update && (
            <section>
              <h3>Update available</h3>
              <p>
                Updating reloads this view. Send or copy unsent drafts first.
              </p>
              <button
                disabled={
                  Object.values(drafts).some(
                    (item) => item.text || item.files.length,
                  ) ||
                  uploading ||
                  Object.values(snapshots).some((item) => item.pending)
                }
                onClick={() => {
                  navigator.serviceWorker.addEventListener(
                    "controllerchange",
                    () => location.reload(),
                    { once: true },
                  );
                  update.postMessage({ type: "ACTIVATE_UPDATE" });
                }}
              >
                Apply update
              </button>
            </section>
          )}
          <details class="settings-section">
            <summary>Notifications</summary>
            <p>
              {catalogue.capabilities.push
                ? "Background push is available for supported installed browsers."
                : "Notifications are available while this view is connected."}
            </p>
            <button
              disabled={
                !online ||
                !isSecureContext ||
                !("Notification" in globalThis) ||
                !("serviceWorker" in navigator)
              }
              onClick={async () => {
                try {
                  if (catalogue.capabilities.subscribed || notificationMode) {
                    await api("/api/push/subscriptions", undefined, {
                      method: "DELETE",
                    });
                    const registration =
                      await navigator.serviceWorker.getRegistration();
                    await (
                      await registration?.pushManager?.getSubscription()
                    )?.unsubscribe();
                    setNotificationMode(false);
                    notifications.current = false;
                  } else {
                    if ((await Notification.requestPermission()) !== "granted")
                      throw new Error(
                        "Notification permission was not granted.",
                      );
                    if (
                      catalogue.capabilities.push &&
                      "PushManager" in globalThis
                    ) {
                      const registration = await navigator.serviceWorker.ready;
                      const key = (
                        catalogue.capabilities.vapid_public_key || ""
                      )
                        .replace(/-/g, "+")
                        .replace(/_/g, "/");
                      const subscription =
                        (await registration.pushManager.getSubscription()) ||
                        (await registration.pushManager.subscribe({
                          userVisibleOnly: true,
                          applicationServerKey: Uint8Array.from(
                            atob(key),
                            (value) => value.charCodeAt(0),
                          ),
                        }));
                      await api(
                        "/api/push/subscriptions",
                        subscription.toJSON(),
                      );
                    } else {
                      setNotificationMode(true);
                      notifications.current = true;
                    }
                  }
                  await refresh();
                } catch (failure) {
                  report(failure);
                }
              }}
            >
              {catalogue.capabilities.subscribed || notificationMode
                ? "Mute this device"
                : catalogue.capabilities.push
                  ? "Enable background notifications"
                  : "Enable connected-view notifications"}
            </button>
            {(catalogue.capabilities.subscribed || notificationMode) && (
              <button
                disabled={!online || !selected}
                onClick={async () => {
                  try {
                    if (catalogue.capabilities.subscribed)
                      await command("test_notification", session);
                    else
                      (await navigator.serviceWorker.ready).active?.postMessage(
                        {
                          type: "ATTENTION",
                          id: crypto.randomUUID(),
                          session_id: selected,
                        },
                      );
                  } catch (failure) {
                    report(failure);
                  }
                }}
              >
                Send test notification
              </button>
            )}
            <p class="muted">
              {catalogue.capabilities.subscribed
                ? "Background notifications enabled for this device."
                : notificationMode
                  ? "Connected-view notifications enabled. Delivery requires this browser view to remain connected."
                  : "Notifications are muted for this device."}{" "}
              Focus settings, power management and network conditions can delay
              delivery. On iOS, enable notifications from the installed Home
              Screen app.
            </p>
            {!catalogue.capabilities.push && (
              <details>
                <summary>Host details</summary>
                <p>{catalogue.capabilities.push_reason}</p>
              </details>
            )}
          </details>
          <details class="settings-section">
            <summary>Paired devices</summary>
            {catalogue.devices.map((device) => (
              <div class="device" key={device.id}>
                <span>
                  {device.name}
                  {device.id === catalogue.device ? " · this device" : ""}
                </span>
                {device.id !== catalogue.device && (
                  <button
                    onClick={() =>
                      command("revoke_device", null, { device_id: device.id })
                        .then(refresh)
                        .catch(report)
                    }
                  >
                    Revoke
                  </button>
                )}
              </div>
            ))}
            <button onClick={logout}>Log out this device</button>
          </details>
        </>
      )}
    </div>
  );
}
