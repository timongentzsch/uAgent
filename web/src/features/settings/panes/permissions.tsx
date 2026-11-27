import { useState } from "preact/hooks";
import type {
  CommandFields,
  PermissionRules,
  RuleEffect,
} from "../../../shared/types.ts";
import {
  Actions,
  Button,
  ConfirmModal,
  Field,
  Group,
  Input,
  InputRow,
  LoadError,
  Row,
} from "../../../shared/ui.tsx";
import { useAction } from "../../../shared/use-action.ts";
import { useResource } from "../../../shared/use-resource.ts";
import { useApi } from "../../../state/api.ts";
import { useSettings } from "../context.ts";

// Never wins over ask, ask over always: listed in that order.
const EFFECTS: [RuleEffect, string][] = [
  ["deny", "Never"],
  ["ask", "Ask"],
  ["allow", "Always"],
];

export function PermissionsPane() {
  const { manage } = useApi();
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
  const [tool, setTool] = useState("run");
  const [match, setMatch] = useState("");
  const [effect, setEffect] = useState<RuleEffect>("ask");
  // Each change answers with the rules left.
  const change = (fields: CommandFields<"permission_rules">) =>
    edit.run(async () =>
      setValue(await manage("permission_rules", { ...fields, cwd })),
    );
  if (!cwd) return null;
  const failure = error ?? edit.error;
  const idle = !online || edit.busy;
  const count = rules?.rules.length ?? 0;
  return (
    <>
      <Group
        title={`Rules${count ? ` · ${count}` : ""}`}
        footer="What this folder lets through, asks about or refuses, in every mode. A never or ask rule covers any part of a command."
      >
        {failure != null && (
          <div class="group-block">
            <LoadError
              error={failure}
              retry={error != null ? retry : undefined}
            />
          </div>
        )}
        {rules && !count && (
          <Row label="No rules. More options on an approval keeps one." />
        )}
        {EFFECTS.map(([kind, name]) => {
          const held = rules?.rules.filter((rule) => rule.effect === kind);
          return held?.map((rule) => (
            <Row
              key={`${rule.tool} ${rule.match}`}
              label={`${rule.tool} ${rule.match === "*" ? "every call" : rule.match}`}
              detail={name}
            >
              <Button
                variant="quiet"
                size="compact"
                disabled={idle}
                onClick={() =>
                  void change({
                    action: "delete",
                    tool: rule.tool,
                    match: rule.match,
                  })
                }
              >
                Forget
              </Button>
            </Row>
          ));
        })}
        {!!count && (
          <Row
            label="Forget all for this folder"
            destructive
            disabled={!online}
            onClick={() => setConfirm(true)}
          />
        )}
      </Group>
      <Group
        title="Add a rule"
        footer="A match is the command, a command and * after it, a folder ending in /, or * for every call."
      >
        <form
          class="group-block rule-form"
          onSubmit={async (event) => {
            event.preventDefault();
            if (await change({ action: "add", tool, match, effect }))
              setMatch("");
          }}
        >
          <InputRow>
            <Field label="Tool">
              <Input
                value={tool}
                required
                onInput={(event) => setTool(event.currentTarget.value.trim())}
              />
            </Field>
            <Field label="Match">
              <Input
                value={match}
                required
                placeholder="terraform apply *"
                onInput={(event) => setMatch(event.currentTarget.value)}
              />
            </Field>
          </InputRow>
          <div class="segmented" role="group" aria-label="Effect">
            {[...EFFECTS].reverse().map(([kind, name]) => (
              <Button
                key={kind}
                aria-pressed={effect === kind}
                onClick={() => setEffect(kind)}
              >
                {name}
              </Button>
            ))}
          </div>
          <Actions>
            <Button type="submit" variant="primary" disabled={idle}>
              Add rule
            </Button>
          </Actions>
        </form>
      </Group>
      {confirm && (
        <ConfirmModal
          title="Forget all"
          busy={edit.busy}
          error={edit.error}
          close={() => setConfirm(false)}
          confirm={async () => {
            if (await change({ action: "clear" })) setConfirm(false);
          }}
        >
          Every rule for this folder is forgotten. The agent asks again the next
          time.
        </ConfirmModal>
      )}
    </>
  );
}
