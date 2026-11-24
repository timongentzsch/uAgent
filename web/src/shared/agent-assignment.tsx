import "./activity.css";
import type { Session } from "./types.ts";
import { agentAssignmentText, agentScope } from "./display.ts";

export function AgentAssignment({
  title,
  name,
  edits,
}: {
  title: string;
  name: string;
  edits?: Session["agent_edits"];
}) {
  const task = title !== name && agentAssignmentText(title);
  const scope = agentScope(edits);
  if (!task && !scope) return null;
  return (
    <details class="agent-assignment">
      <summary
        class="small muted"
        aria-label={
          task ? `Assignment for ${name}. ${task}` : `Access for ${name}`
        }
      >
        <span class="truncate">{task || "Access"}</span>
      </summary>
      {scope && <small class="muted">{scope}</small>}
    </details>
  );
}
