import { useEffect, useState } from "preact/hooks";
import type { ComponentProps } from "preact";
import type Settings from "../features/settings/settings.tsx";
import ApprovalControl from "../features/composer/approval-control.tsx";
import ContextSection from "../features/chat/context-parts.tsx";
import { StatisticsContent } from "../features/chat/statistics.tsx";
import Tools from "../features/settings/tools.tsx";
import { RemoteHostsPane } from "../features/sidebar/hosts.tsx";
import ModelControl from "../shared/model-control.tsx";
import { capitalize, valueSources } from "../shared/display.ts";
import { useApi } from "../state/api.ts";
import { useHostContext } from "../state/host-context.ts";
import type { Session } from "../shared/types.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { Button, Group, LoadError, Row, Switch } from "../shared/ui.tsx";
import { useAction } from "../shared/use-action.ts";
import { restartConversation, type DetailControl } from "./menus.tsx";

type Panels = ComponentProps<typeof Settings>["panels"];

// What Settings shows of the open conversation, drawn with the same
// controls the composer and the menus use.
export function settingsPanels({
  session,
  detail,
}: {
  session?: Session;
  detail: DetailControl;
}): Panels {
  const conversation: Panels = session
    ? {
        context: () => <ConversationContext session={session} />,
        model: (go) => <Model session={session} go={go} />,
        approval: (go) => <Approval session={session} go={go} />,
        tools: (_, row) => <ConversationTools session={session} row={row} />,
        view: () => <View detail={detail} />,
        usage: () => <Usage session={session} />,
        restart: () => <Restart session={session} />,
      }
    : {};
  return { ...conversation, hosts: () => <RemoteHostsPane /> };
}

// Each panel reads only what it shows, so a streamed frame redraws it and
// not Settings around it.
function useConversationState(session: Session) {
  const { store } = useHostContext();
  const state = useSnapshots(store, (all) => all[session.id]?.state);
  const pending = useSnapshots(store, (all) => !!all[session.id]?.pending);
  return { state, pending };
}

function Usage({ session }: { session: Session }) {
  const { state } = useConversationState(session);
  return (
    <Group title="Usage">
      <div class="group-block">
        <StatisticsContent session={session} state={state} context={false} />
      </div>
    </Group>
  );
}

function ConversationContext({ session }: { session: Session }) {
  const { showContext } = useHostContext();
  const { state } = useConversationState(session);
  return (
    <div class="group-block">
      <ContextSection
        key={session.id}
        session={session}
        state={state}
        showContext={() => showContext()}
      />
    </div>
  );
}

function Model({
  session,
  go,
}: {
  session: Session;
  go: (address: string) => void;
}) {
  const { online } = useHostContext();
  const { state, pending } = useConversationState(session);
  return (
    <Group>
      <Row label="Model" detail={valueSources[state?.route_source ?? ""]}>
        <ModelControl
          session={session}
          state={state}
          online={online}
          running={!!session.turn_active || pending}
          settings={go}
        />
      </Row>
    </Group>
  );
}

function Approval({
  session,
  go,
}: {
  session: Session;
  go: (address: string) => void;
}) {
  const { online, report } = useHostContext();
  const { state } = useConversationState(session);
  return (
    <Group>
      <Row
        label="Approval"
        detail={valueSources[state?.permissions?.source ?? ""]}
      >
        <ApprovalControl
          session={session}
          permission={state?.permissions}
          online={online}
          report={report}
          settings={go}
        />
      </Row>
    </Group>
  );
}

function ConversationTools({
  session,
  row,
}: {
  session: Session;
  row?: string;
}) {
  const { online, load } = useHostContext();
  const { pending } = useConversationState(session);
  // Open when Settings was asked for this row (/tools, the menu).
  const [open, setOpen] = useState(row === "tools");
  useEffect(() => {
    if (row === "tools") setOpen(true);
  }, [row]);
  return (
    <Group>
      <Row
        label="Tools"
        detail="Which tools this conversation is offered."
        expanded={open}
        onClick={() => setOpen(!open)}
      />
      {open && (
        <div class="group-block">
          <Tools
            session={session}
            online={online}
            busy={!!session.turn_active || pending}
            changed={() => void load(session.id)}
          />
        </div>
      )}
    </Group>
  );
}

function View({ detail }: { detail: DetailControl }) {
  return (
    <Group footer="On this device.">
      <Row label="View" detail="How much of the agent's work is shown.">
        <div class="segmented" role="group" aria-label="View">
          {detail.levels.map((level) => (
            <Button
              key={level}
              aria-pressed={level === detail.level}
              onClick={() => detail.change(level)}
            >
              {capitalize(level)}
            </Button>
          ))}
        </div>
      </Row>
      <Row label="Thinking" detail="The model's thinking in the transcript.">
        <Switch
          label="Thinking"
          checked={detail.thinking}
          onChange={detail.showThinking}
        />
      </Row>
    </Group>
  );
}

function Restart({ session }: { session: Session }) {
  const { command, manage } = useApi();
  const { online, load } = useHostContext();
  const action = useAction();
  const [done, setDone] = useState("");
  const running = !!session.generation;
  return (
    <Group>
      <Row
        label="Restart conversation"
        detail={done || "Same history, a fresh process."}
        disabled={!online || !running || action.busy}
        onClick={() =>
          void action.run(async () =>
            setDone(await restartConversation(session, manage)),
          )
        }
      />
      <Row
        label="Close conversation"
        detail="Stops it until the next message."
        disabled={!online || !running || action.busy}
        onClick={() =>
          void action.run(async () => {
            await command("close", session);
            await load(session.id);
          })
        }
      />
      {action.error != null && <LoadError error={action.error} />}
    </Group>
  );
}
