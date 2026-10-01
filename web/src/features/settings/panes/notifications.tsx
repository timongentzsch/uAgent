import { Group, LoadError, Row, Switch } from "../../../shared/ui.tsx";
import { useAction } from "../../../shared/use-action.ts";
import { command } from "../../../state/api.ts";
import { subscribePush, unsubscribePush } from "../../../state/push.ts";
import { useSettings } from "../context.ts";

// Notifications for this device: background push where the host offers it,
// otherwise notifications while this view stays connected.
export function NotificationsPane() {
  const {
    catalogue,
    online,
    notificationMode,
    setNotificationMode,
    notifications,
    refresh,
    selected,
    session,
  } = useSettings();
  const { push, subscribed, push_reason, vapid_public_key } =
    catalogue.capabilities;
  const action = useAction();
  const on = !!subscribed || notificationMode;
  const connected = (enabled: boolean) => {
    setNotificationMode(enabled);
    notifications.current = enabled;
  };
  const set = (enable: boolean) =>
    action.run(async () => {
      if (!enable) {
        await unsubscribePush();
        connected(false);
      } else {
        if ((await Notification.requestPermission()) !== "granted")
          throw new Error("Notification permission was not granted.");
        if (push && "PushManager" in globalThis)
          await subscribePush(vapid_public_key || "");
        else connected(true);
      }
      await refresh();
    });
  const test = () =>
    action.run(async () => {
      if (subscribed) await command("test_notification", session);
      else
        (await navigator.serviceWorker.ready).active?.postMessage({
          type: "ATTENTION",
          id: crypto.randomUUID(),
          session_id: selected,
        });
    });
  return (
    <Group
      title="Notifications"
      footer={
        <>
          {subscribed
            ? "Background notifications enabled for this device."
            : notificationMode
              ? "Connected-view notifications enabled. Delivery requires this browser view to remain connected."
              : "Notifications are muted for this device."}{" "}
          Focus settings, power management and network conditions can delay
          delivery. On iOS, enable notifications from the installed Home Screen
          app.
        </>
      }
    >
      {action.error != null && (
        <div class="group-block">
          <LoadError error={action.error} />
        </div>
      )}
      <Row
        label="Notifications"
        detail={
          push
            ? "Background push, for supported installed browsers."
            : "While this view is connected."
        }
      >
        <Switch
          label="Notifications"
          checked={on}
          disabled={
            !online ||
            action.busy ||
            !isSecureContext ||
            !("Notification" in globalThis) ||
            !("serviceWorker" in navigator)
          }
          onChange={(enable) => void set(enable)}
        />
      </Row>
      {on && (
        <Row
          label="Send test notification"
          disabled={!online || !selected}
          onClick={() => void test()}
        />
      )}
      {!push && <Row label="Why no background push" detail={push_reason} />}
    </Group>
  );
}
