import type { ComponentChildren } from "preact";
import { Check } from "lucide-preact";
import type {
  AppModal,
  Session,
  Space,
  TerminalInfo,
} from "../shared/types.ts";
import { useApi, type ApiClient } from "../state/api.ts";
import { useHostContext } from "../state/host-context.ts";
import { Menu, MenuItem, MenuSub } from "../shared/menu.tsx";
import { capitalize } from "../shared/display.ts";
import { PinItem } from "../features/sidebar/pins.tsx";

export async function shareTranscript(
  item: Session,
  command: ApiClient["command"],
) {
  const result = await command("share", item);
  return result.pending ? "" : `Transcript saved to ${result.result.path}`;
}
// A fresh runtime that keeps the history.
export async function restartConversation(
  item: Session,
  manage: ApiClient["manage"],
) {
  const { restarting, deferred } = await manage("restart_conversations", {
    target_id: item.id,
  });
  return restarting
    ? "Conversation restarted."
    : deferred
      ? "Restarts when this turn ends."
      : "Nothing to restart: this conversation is not running.";
}

export function SpaceMenu({
  space,
  conversations,
  open,
}: {
  space: Space;
  conversations: number;
  open: (modal: AppModal) => void;
}) {
  const { online } = useHostContext();
  return (
    <Menu label={`Menu for the workspace ${space.name}`}>
      <PinItem pin={{ kind: "folder", id: space.path }} />
      <MenuItem
        disabled={!online}
        onClick={() => open({ type: "rename", space })}
      >
        Rename
      </MenuItem>
      <MenuItem
        disabled={!online}
        onClick={() => open({ type: "delete", space, conversations })}
      >
        Delete
      </MenuItem>
    </Menu>
  );
}

export function TerminalMenu({
  terminal,
  open,
  close,
}: {
  terminal: TerminalInfo;
  open: (modal: AppModal) => void;
  close: (terminal: TerminalInfo) => void;
}) {
  const { online } = useHostContext();
  return (
    <Menu label={`Terminal menu for ${terminal.title}`}>
      <PinItem pin={{ kind: "terminal", id: terminal.id }} />
      <MenuItem
        disabled={!online}
        onClick={() => open({ type: "rename", terminal })}
      >
        Rename
      </MenuItem>
      <MenuItem disabled={!online} onClick={() => close(terminal)}>
        Close
      </MenuItem>
    </Menu>
  );
}

export function ConversationMenu({
  item,
  fork,
  find,
  notify,
  open,
  detail,
  children,
  more,
  select,
}: {
  item: Session;
  fork: (item: Session) => void;
  find?: () => void;
  notify: (text: string) => void;
  open: (modal: AppModal) => void;
  detail?: {
    levels: readonly string[];
    level: string;
    change: (level: string) => void;
  };
  children?: ComponentChildren;
  more?: ComponentChildren;
  select?: () => void;
}) {
  const { command, manage } = useApi();
  const { online, load: loadSnapshot, report } = useHostContext();
  return (
    <Menu label="Conversation menu">
      {children}
      {find && <MenuItem onClick={find}>Find in conversation</MenuItem>}
      {!item.parent && <PinItem pin={{ kind: "session", id: item.id }} />}
      <MenuItem
        disabled={!online}
        onClick={() => {
          open({ type: "rename", session: item });
        }}
      >
        Rename
      </MenuItem>
      {!item.parent && item.status !== "draft" && (
        <MenuItem
          disabled={!online}
          onClick={() => open({ type: "move", session: item })}
        >
          Move to…
        </MenuItem>
      )}
      {select && <MenuItem onClick={select}>Select</MenuItem>}
      <MenuSub label="More">
        {more}
        <MenuItem
          disabled={
            !online ||
            item.turn_active ||
            ["starting", "draft"].includes(item.status || "")
          }
          title={
            item.turn_active ||
            ["starting", "draft"].includes(item.status || "")
              ? "Wait for the running turn to finish before forking"
              : undefined
          }
          onClick={() => fork(item)}
        >
          Fork conversation
        </MenuItem>
        <MenuItem
          onClick={() => open({ type: "statistics", session_id: item.id })}
        >
          Statistics
        </MenuItem>
        <MenuItem
          disabled={!online}
          onClick={() => open({ type: "tools", session_id: item.id })}
        >
          Tools
        </MenuItem>
        <MenuItem
          disabled={!online}
          onClick={() =>
            void shareTranscript(item, command).then(notify, report)
          }
        >
          Export transcript
        </MenuItem>
        {detail && detail.levels.length > 0 && (
          <MenuSub label="Detail" value={capitalize(detail.level)}>
            {detail.levels.map((level) => (
              <MenuItem
                key={level}
                role="menuitemradio"
                aria-checked={level === detail.level}
                onClick={() => detail.change(level)}
              >
                {capitalize(level)}
                {level === detail.level && <Check />}
              </MenuItem>
            ))}
            <p class="menu-note small">On this device.</p>
          </MenuSub>
        )}
        {item.generation && (
          <MenuItem
            disabled={!online || item.turn_active}
            onClick={() =>
              void command("submit", item, { text: "/compact" }).catch(report)
            }
          >
            Compact
          </MenuItem>
        )}
        {item.generation && (
          <MenuItem
            disabled={!online}
            onClick={() =>
              void restartConversation(item, manage).then(notify, report)
            }
          >
            Restart
          </MenuItem>
        )}
        {item.generation && (
          <MenuItem
            disabled={!online}
            onClick={() =>
              command("close", item)
                .then(() => loadSnapshot(item.id))
                .catch(report)
            }
          >
            Close conversation
          </MenuItem>
        )}
        <MenuItem
          disabled={!online}
          onClick={() => {
            open({ type: "delete", session: item });
          }}
        >
          Delete
        </MenuItem>
      </MenuSub>
    </Menu>
  );
}
