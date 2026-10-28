import { useState } from "preact/hooks";
import { Field, Input } from "./ui.tsx";

// A folder chosen by typing a path or picking one the host already lists.

export function ProjectField({
  value,
  projects,
  change,
}: {
  value: string;
  projects: string[];
  change: (value: string) => void;
}) {
  // Null while not typing: the field then shows the chosen folder.
  const [draft, setDraft] = useState<string | null>(null);
  return (
    <Field label="Folder">
      <Input
        list="management-projects"
        value={draft ?? value}
        onInput={(event) => setDraft(event.currentTarget.value)}
        placeholder="Absolute directory on the host"
        onChange={(event) => {
          change(event.currentTarget.value);
          setDraft(null);
        }}
      />
      <datalist id="management-projects">
        {projects.map((path) => (
          <option key={path} value={path} />
        ))}
      </datalist>
    </Field>
  );
}
