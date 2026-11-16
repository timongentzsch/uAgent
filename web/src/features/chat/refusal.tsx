import { useContext, useState } from "preact/hooks";
import type { Node, RefusalPart, SessionRef } from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import { cleanText } from "../../shared/display.ts";
import { HostContext } from "../../state/host-context.ts";
import { useApi } from "../../state/api.ts";
import { headlineOf } from "../../shared/tree.ts";

export const refusalOf = (node: Node) =>
  node.parts?.find((part): part is RefusalPart => part.kind === "refusal");

// A call refused without asking anyone: what and why, and what lets it
// through: the action itself for this conversation, or the folder's rules.
export function Refusal({
  node,
  part,
  online,
  session,
}: {
  node: Node;
  part: RefusalPart;
  online: boolean;
  session: SessionRef;
}) {
  const { command } = useApi();
  const host = useContext(HostContext);
  const [allowed, setAllowed] = useState(false);
  const [busy, setBusy] = useState(false);
  const { offer } = part;
  return (
    <div class="refusal" role="note">
      <div class="refusal-text">
        <span>
          <strong class="refusal-word">Refused</strong>{" "}
          {cleanText(headlineOf(node))}
        </span>
        <small class="muted">{cleanText(part.reason)}</small>
      </div>
      {offer ? (
        <Button
          disabled={!online || busy || allowed}
          onClick={async () => {
            setBusy(true);
            try {
              await command("permissions", session, { allow: offer });
              setAllowed(true);
            } catch (error) {
              host?.report(error, "inline");
            } finally {
              setBusy(false);
            }
          }}
        >
          {allowed
            ? "Allowed in this conversation"
            : "Allow in this conversation"}
        </Button>
      ) : (
        host && (
          <Button
            onClick={() =>
              host.setModal({ type: "settings", section: "project" })
            }
          >
            Rules…
          </Button>
        )
      )}
    </div>
  );
}
