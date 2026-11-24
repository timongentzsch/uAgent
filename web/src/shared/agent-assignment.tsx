import "./activity.css";
import { agentAssignmentText } from "./display.ts";

export function AgentAssignment({
  title,
  name,
}: {
  title: string;
  name: string;
}) {
  const task = title !== name && agentAssignmentText(title);
  if (!task) return null;
  return (
    <details class="agent-assignment">
      <summary
        class="small muted"
        aria-label={`Assignment for ${name}. ${task}`}
      >
        <span class="truncate">{task}</span>
      </summary>
    </details>
  );
}
