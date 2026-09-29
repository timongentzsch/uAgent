import { useEffect, useState } from "preact/hooks";
import type { SoulDocuments } from "../../shared/types.ts";
import { command } from "../../state/api.ts";
import { Actions, Button, LoadError, Textarea } from "../../shared/ui.tsx";
import "./soul.css";

// The coordinator's soul inside the system prompt editor: plain standing
// guidance, no modes. Yours applies in every folder; a project's adds to it
// once that project's config is trusted.
export default function SoulEditor({
  scope,
  cwd,
  online,
}: {
  scope: "user" | "project";
  cwd: string;
  online: boolean;
}) {
  const [souls, setSouls] = useState<SoulDocuments>();
  const [text, setText] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const request = async (fields: Record<string, string>) => {
    const result = await command("soul", null, { cwd, ...fields });
    if (result.pending) throw new Error("Reload to see the saved soul.");
    return result.result;
  };
  useEffect(() => {
    if (!online) return;
    setError(null);
    request({ action: "show" })
      .then((value) => {
        setSouls(value);
        setText(value[scope].text);
      })
      .catch(setError);
  }, [cwd, scope, online]);
  const current = souls?.[scope];
  const changed = !!current && text !== current.text;
  async function save() {
    setBusy(true);
    setError(null);
    try {
      const value = await request({ action: "set", scope, text });
      setSouls(value);
      setText(value[scope].text);
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  }
  return (
    <section class="soul-editor">
      <p class="muted">
        Standing guidance each folder's coordinator reads every turn, after its
        base prompt. Conversations never read it.
      </p>
      {error && <LoadError error={error} />}
      <Textarea
        aria-label="Soul text"
        value={text}
        placeholder="For example: prefer small threads; ask before touching CI."
        disabled={!souls || busy || !online}
        onInput={(event) => setText(event.currentTarget.value)}
      />
      <small class="muted">
        {current?.path}
        {scope === "project" &&
          souls &&
          !souls.project.loaded &&
          " · not loaded until this project's config is trusted"}
      </small>
      <Actions>
        <Button
          variant="secondary"
          disabled={!changed || busy}
          onClick={() => setText(current?.text || "")}
        >
          Discard
        </Button>
        <Button
          variant="primary"
          disabled={!changed || busy || !online}
          onClick={save}
        >
          Save
        </Button>
      </Actions>
    </section>
  );
}
