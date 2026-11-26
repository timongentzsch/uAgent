import { Shield } from "lucide-preact";
import {
  Button,
  Field,
  Select,
  DataText,
  LoadError,
} from "../../shared/ui.tsx";
import { SheetButton } from "../../shared/sheet.tsx";
import { useResource } from "../../shared/use-resource.ts";
import { useApi } from "../../state/api.ts";
import type { Session, State, Report } from "../../shared/types.ts";
import {
  agentScope,
  valueSources,
  permissionLabel,
  permissionLabels,
  permissionNotes,
} from "../../shared/display.ts";

// Read the running worker only when the approval sheet is open. Its execution
// host knows the enforced policy, including platform-specific network limits.
function CommandAccess({
  session,
  effective,
}: {
  session: Session;
  effective?: string;
}) {
  const { command } = useApi();
  const read = useResource(
    async (signal) => {
      const receipt = await command(
        "permissions",
        session,
        { inspect: true },
        { signal },
      );
      if (receipt.pending)
        throw new Error("Command access is still loading. Try again.");
      return receipt.result?.sandbox;
    },
    [session.id, session.generation, effective],
  );
  if (read.error) return <LoadError error={read.error} retry={read.retry} />;
  const sandbox = read.value;
  return sandbox?.summary ? (
    <p class="small">
      Command sandbox · {sandbox.summary}
      {sandbox.network === "allowed" ? " · Network allowed" : ""}
    </p>
  ) : null;
}

export default function ApprovalControl({
  session,
  permission,
  online,
  report,
  settings,
}: {
  session: Session;
  permission?: State["permissions"];
  online: boolean;
  report: Report;
  // Opens Settings at an address: the default's row.
  settings: (address: string) => void;
}) {
  const { command } = useApi();
  const effective =
    permission?.effective ??
    (permission?.mode === "default" ? permission.default : permission?.mode);
  const source = valueSources[permission?.source ?? ""];
  const effectiveLabel = permissionLabel(effective);
  return (
    <SheetButton
      label="Approval"
      title={[effectiveLabel, source].filter(Boolean).join(" · ")}
      className={`permission-control${effective === "yolo" ? " yolo" : ""}`}
      disabled={!online}
      trigger={
        <>
          <Shield />
          <span>
            <DataText>{effectiveLabel}</DataText>
          </span>
        </>
      }
    >
      {(close) => (
        <>
          <Field
            label="This conversation's approval mode"
            help={[
              agentScope(session.agent_edits),
              permissionNotes[effective ?? "ask"] ?? permissionNotes.ask,
              "Changes to protected settings and startup files, unsandboxed commands and root commands require your approval.",
            ]
              .filter(Boolean)
              .join(" · ")}
          >
            <Select
              aria-label="Approval"
              value={permission?.mode || "default"}
              onChange={(event) =>
                command("permissions", session, {
                  mode: event.currentTarget.value,
                })
                  .then(close)
                  .catch(report)
              }
            >
              <option value="default">
                Default ({permissionLabel(permission?.default)})
              </option>
              {Object.entries(permissionLabels).map(([value, label]) => (
                <option value={value}>{label}</option>
              ))}
            </Select>
          </Field>
          {session.presence === "active" && (
            <CommandAccess session={session} effective={effective} />
          )}
          <div class="default-source">
            <small class="muted">{source}</small>
            <Button
              variant="quiet"
              size="compact"
              onClick={() => {
                close();
                settings("approvals/approval.mode");
              }}
            >
              Change the default
            </Button>
          </div>
        </>
      )}
    </SheetButton>
  );
}
