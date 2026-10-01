import { useState } from "preact/hooks";
import type { CommandFields, PermissionRules } from "../../../shared/types.ts";
import {
  Button,
  ConfirmModal,
  Group,
  LoadError,
  Row,
} from "../../../shared/ui.tsx";
import { useAction } from "../../../shared/use-action.ts";
import { useResource } from "../../../shared/use-resource.ts";
import { manage } from "../../../state/api.ts";
import { useSettings } from "../context.ts";

// The exact actions remembered for the open conversation's repository.
export function PermissionsPane() {
  const { session, online } = useSettings();
  const cwd = session?.cwd;
  const {
    value: rules,
    error,
    retry,
    setValue,
  } = useResource<PermissionRules>(
    () =>
      online && cwd
        ? manage("permission_rules", { action: "list", cwd })
        : undefined,
    [online, cwd],
  );
  const edit = useAction();
  const [confirm, setConfirm] = useState(false);
  // Forgetting one rule or all of them answers with the rules left.
  const forget = (fields: CommandFields) =>
    edit.run(async () =>
      setValue(await manage("permission_rules", { ...fields, cwd })),
    );
  if (!cwd) return null;
  const failure = error ?? edit.error;
  return (
    <Group
      title={`Allowed actions${rules?.rules.length ? ` · ${rules.rules.length}` : ""}`}
      footer="Exact actions allowed for this repository. Tool definitions and arguments must still match."
    >
      {failure != null && (
        <div class="group-block">
          <LoadError
            error={failure}
            retry={error != null ? retry : undefined}
          />
        </div>
      )}
      {rules?.rules.map((rule) => (
        <Row key={rule.key} label={rule.tool} detail={rule.preview}>
          <Button
            variant="quiet"
            size="compact"
            disabled={!online || edit.busy}
            onClick={() => void forget({ action: "delete", key: rule.key })}
          >
            Forget
          </Button>
        </Row>
      ))}
      {rules && !rules.rules.length && <Row label="No remembered actions." />}
      {!!rules?.rules.length && (
        <Row
          label="Forget all for this repository"
          destructive
          disabled={!online}
          onClick={() => setConfirm(true)}
        />
      )}
      {confirm && (
        <ConfirmModal
          title="Forget all"
          busy={edit.busy}
          error={edit.error}
          close={() => setConfirm(false)}
          confirm={async () => {
            if (await forget({ action: "clear" })) setConfirm(false);
          }}
        >
          Every remembered action for this repository is forgotten. The agent
          asks again the next time.
        </ConfirmModal>
      )}
    </Group>
  );
}
