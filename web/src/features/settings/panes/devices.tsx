import { useState } from "preact/hooks";
import {
  Button,
  ConfirmModal,
  Group,
  LoadError,
  Row,
} from "../../../shared/ui.tsx";
import { useAction } from "../../../shared/use-action.ts";
import { manage } from "../../../state/api.ts";
import { useSettings } from "../context.ts";
import { NotificationsPane } from "./notifications.tsx";

// The display mode and the device are fixed for the page's life.
const installed =
  matchMedia("(display-mode: standalone)").matches ||
  ("standalone" in navigator && navigator.standalone === true);
const ios =
  /iPhone|iPad/.test(navigator.userAgent) ||
  (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);

export function DevicesPane() {
  const { install, setInstall, catalogue, refresh, logout } = useSettings();
  const action = useAction();
  const [confirm, setConfirm] = useState<
    { id: string; name: string } | "logout" | null
  >(null);
  return (
    <>
      {action.error != null && !confirm && <LoadError error={action.error} />}
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
