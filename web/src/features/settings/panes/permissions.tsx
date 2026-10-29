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

// The rules kept for the open conversation's repository.
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
  const forget = (fields: CommandFields<"permission_rules">) =>
    edit.run(async () =>
      setValue(await manage("permission_rules", { ...fields, cwd })),
    );
  if (!cwd) return null;
  const failure = error ?? edit.error;
  return (
    <Group
      title={`Allowed actions${rules?.rules.length ? ` · ${rules.rules.length}` : ""}`}
      footer="Calls allowed without asking in this repository."
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
        <Row
          key={`${rule.tool} ${rule.match}`}
          label={rule.tool}
          detail={rule.match === "*" ? "every call" : rule.match}
        >
          <Button
            variant="quiet"
            size="compact"
            disabled={!online || edit.busy}
            onClick={() =>
              void forget({
                action: "delete",
                tool: rule.tool,
                match: rule.match,
              })
            }
          >
            Forget
          </Button>
        </Row>
      ))}
      {rules && !rules.rules.length && (
        <Row label="No rules. “Always allow here” on an approval adds one." />
      )}
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
          Every rule for this repository is forgotten. The agent asks again the
          next time.
        </ConfirmModal>
      )}
    </Group>
  );
}
