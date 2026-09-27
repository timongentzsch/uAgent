import { useState } from "preact/hooks";
import type { McpServer, Session, State } from "../../shared/types.ts";
import { Button, Group, LoadError, Row, Switch } from "../../shared/ui.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { command } from "../../state/api.ts";
import { plural } from "../../shared/quantities.ts";

const SUMMARY: Record<McpServer["state"], string> = {
  ready: "Ready",
  starting: "Starting…",
  disabled: "Disabled",
  failed: "Failed",
};

// The open conversation's servers, grouped by the file that defines them, so
// it is always clear whether a switch edits the global or the project config.
export function McpServers({
  session,
  state,
  online,
}: {
  session?: Session;
  state?: State;
  online: boolean;
}) {
  const [open, setOpen] = useState("");
  const [busy, setBusy] = useState("");
  // The switch shows the requested value while the server starts or stops.
  const [wanted, setWanted] = useState<boolean | null>(null);
  const [error, setError] = useState<unknown>(null);
  if (!session)
    return (
      <Group footer="Servers start with a conversation, so their state is that conversation's.">
        <Row label="Open a conversation to see its MCP servers" />
      </Group>
    );
  const servers = state?.mcp || [];
  const control = async (name: string, fields: Record<string, unknown>) => {
    setBusy(name);
    setError(null);
    try {
      await command("tools", session, { name, ...fields });
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy("");
      setWanted(null);
    }
  };
  const project = session.cwd?.split("/").pop() || "project";
  const scopes = [
    ["global", "Global · all projects", "~/.mcp.json"],
    ["project", `This project · ${project}`, `${session.cwd}/.mcp.json`],
  ] as const;
  return (
    <>
      {error !== null && <LoadError error={error} />}
      {scopes.map(([scope, title, file]) => {
        const rows = servers.filter((server) => server.scope === scope);
        return (
          <Group key={scope} title={title} footer={file}>
            {!rows.length && (
              <Row
                label={
                  scope === "global"
                    ? "No global servers"
                    : "No project servers"
                }
              />
            )}
            {rows.map((server) => {
              const expanded = open === server.name;
              const detail = [
                server.state === "ready"
                  ? plural(server.tools, "tool")
                  : server.state === "failed"
                    ? server.error
                    : SUMMARY[server.state],
                server.overrides && "overrides global",
              ]
                .filter(Boolean)
                .join(" · ");
              return [
                <Row
                  key={server.name}
                  label={
                    <span class="mcp-name">
                      <StatusLed
                        state={
                          server.state === "ready"
                            ? "active"
                            : server.state === "starting"
                              ? "running"
                              : server.state === "failed"
                                ? "failed"
                                : "idle"
                        }
                      />
                      {server.name}
                    </span>
                  }
                  detail={detail}
                  expanded={expanded}
                  onClick={() => setOpen(expanded ? "" : server.name)}
                />,
                expanded && (
                  <div key={`${server.name}:detail`} class="mcp-detail">
                    <code>{server.command}</code>
                    {server.log && <pre class="mcp-log">{server.log}</pre>}
                    <Row
                      label="Enabled"
                      detail={`Edits ${scope === "global" ? "~/.mcp.json" : server.file}`}
                    >
                      <Switch
                        label={`Enable ${server.name}`}
                        checked={
                          busy === server.name && wanted !== null
                            ? wanted
                            : server.state !== "disabled"
                        }
                        disabled={!online || busy !== ""}
                        onChange={(enabled) => {
                          setWanted(enabled);
                          void control(server.name, {
                            operation: "mcp_enable",
                            enabled,
                          });
                        }}
                      />
                    </Row>
                    {server.state === "failed" && (
                      <Button
                        busy={busy === server.name}
                        disabled={!online || busy !== ""}
                        onClick={() =>
                          void control(server.name, {
                            operation: "mcp_restart",
                          })
                        }
                      >
                        Retry
                      </Button>
                    )}
                  </div>
                ),
              ];
            })}
          </Group>
        );
      })}
    </>
  );
}
