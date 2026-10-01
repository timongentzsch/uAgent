import { useState } from "preact/hooks";
import {
  Button,
  ConfirmModal,
  Group,
  LoadError,
  Row,
} from "../../../shared/ui.tsx";
import { applyUpdate } from "../../../shared/pwa.ts";
import { useAction } from "../../../shared/use-action.ts";
import { manage } from "../../../state/api.ts";
import { useSettings } from "../context.ts";
import { NotificationsPane } from "./notifications.tsx";

// This device and the others paired with the host: updating and installing
// the app, notifications, and who may connect.
export function DevicesPane() {
  const {
    update,
    updateBlocked,
    installed,
    install,
    setInstall,
    ios,
    catalogue,
    refresh,
    logout,
  } = useSettings();
  const action = useAction();
  // What is being confirmed: a device to revoke, or logging this one out.
  const [confirm, setConfirm] = useState<
    { id: string; name: string } | "logout" | null
  >(null);
  return (
    <>
      {action.error != null && !confirm && <LoadError error={action.error} />}
      {update && (
        <Group
          title="Update"
          footer="Updating reloads this view. Send or copy unsent drafts first."
        >
          <Row
            label="Apply update"
            disabled={updateBlocked}
            onClick={() => applyUpdate(update)}
          />
        </Group>
      )}
      <Group title="Install">
        {installed ? (
          <Row label="Installed" detail="Running as an installed app." />
        ) : install ? (
          <Row
            label="Install µAgent"
            onClick={() =>
              void action
                .run(() => install.prompt())
                .then(() => setInstall(null))
            }
          />
        ) : (
          <Row
            label="Install"
            detail={
              ios
                ? "In Safari, choose Share → Add to Home Screen. Open the installed app and pair it if needed."
                : "Use your browser’s install option where supported. Installation needs a trusted HTTPS origin or localhost."
            }
          />
        )}
      </Group>
      <NotificationsPane />
      <Group title="Paired devices">
        {catalogue.devices.map((device) => (
          <Row
            key={device.id}
            label={device.name}
            detail={device.id === catalogue.device ? "This device" : undefined}
          >
            {device.id !== catalogue.device && (
              <Button
                variant="quiet"
                size="compact"
                onClick={() => setConfirm(device)}
              >
                Revoke
              </Button>
            )}
          </Row>
        ))}
        <Row
          label="Log out this device"
          destructive
          onClick={() => setConfirm("logout")}
        />
      </Group>
      {confirm === "logout" ? (
        <ConfirmModal
          title="Log out"
          busy={action.busy}
          error={action.error}
          close={() => setConfirm(null)}
          confirm={() => void action.run(logout)}
        >
          This device is unpaired. Pairing it again needs a new code from the
          host.
        </ConfirmModal>
      ) : (
        confirm && (
          <ConfirmModal
            title="Revoke"
            busy={action.busy}
            error={action.error}
            close={() => setConfirm(null)}
            confirm={async () => {
              const revoked = await action.run(async () => {
                await manage("revoke_device", { device_id: confirm.id });
                await refresh();
              });
              if (revoked) setConfirm(null);
            }}
          >
            {confirm.name} can no longer connect until it is paired again.
          </ConfirmModal>
        )
      )}
    </>
  );
}
