import "./tools.css";
import { useMemo, useState } from "preact/hooks";
import type {
  CommandInputs,
  Session,
  ToolCatalogue,
  ToolCatalogueItem,
} from "../../shared/types.ts";
import {
  DataText,
  Input,
  LoadError,
  Placeholder,
  Select,
} from "../../shared/ui.tsx";
import { command } from "../../state/api.ts";
import { bytes } from "../../shared/quantities.ts";
import { capitalize } from "../../shared/display.ts";
import { useResource } from "../../shared/use-resource.ts";
import { matches } from "../../shared/fuzzy.ts";

// The Tools dialog: which tools a conversation may call, by profile or one
// by one, and what they cost in schema bytes.

const labels: Record<string, string> = {
  workspace: "Files",
  execute: "Execute",
  web: "Web",
  collaborate: "Collaborate",
  memory: "Memory and skills",
  system: "System",
  mcp: "MCP",
};

const categoryOrder = Object.keys(labels);

// What the list draws while the catalogue loads (see <Placeholder>).
const SAMPLE: ToolCatalogue = {
  profile: "default",
  base_profile: "default",
  profiles: ["default"],
  active: 12,
  available: 12,
  schema_bytes: 12000,
  full_schema_bytes: 12000,
  tools: ["Read Path", "Write File", "Edit File", "Delete File"].map(
    (title) => ({
      name: title.toLowerCase().replace(" ", "_"),
      title,
      description: "What the tool does and when the agent should use it.",
      category: "workspace",
      provider: "builtin",
      active: true,
      available: true,
      schema_bytes: 500,
    }),
  ),
};

export default function Tools({
  session,
  online,
  busy,
  changed,
}: {
  session: Session;
  online: boolean;
  busy: boolean;
  changed: () => void;
}) {
  const {
    value: catalogue,
    error,
    setValue: setCatalogue,
    setError,
  } = useResource<ToolCatalogue>(async () => {
    const response = await command("tools", session, { operation: "catalog" });
    if (response.pending)
      throw new Error("Tool settings are still loading. Try again shortly.");
    return response.result;
  }, [session.id, session.generation]);
  const [query, setQuery] = useState("");
  const [saving, setSaving] = useState("");

  const update = async (fields: CommandInputs["tools"]) => {
    setError(null);
    setSaving(fields.name || fields.profile || fields.operation);
    try {
      const response = await command("tools", session, fields);
      if (response.pending)
        throw new Error("Tool settings are still loading. Try again shortly.");
      setCatalogue(response.result);
      changed();
    } catch (failure) {
      setError(failure);
    } finally {
      setSaving("");
    }
  };

  const groups = useMemo(() => {
    const found = new Map<string, ToolCatalogueItem[]>();
    for (const tool of (catalogue ?? SAMPLE).tools) {
      const category = tool.category || "workspace";
      if (
        !matches(
          query,
          tool.name,
          tool.title,
          tool.description,
          tool.provider,
          labels[category] || category,
        )
      )
        continue;
      if (!found.has(category)) found.set(category, []);
      found.get(category)!.push(tool);
    }
    const position = (id: string) => {
      const at = categoryOrder.indexOf(id);
      return at < 0 ? categoryOrder.length : at;
    };
    return [...found].sort(([a], [b]) => position(a) - position(b));
  }, [catalogue, query]);

  if (!catalogue && error) return <LoadError error={error} />;
  const shown = catalogue ?? SAMPLE;
  const locked = !online || busy || !!saving;
  const saved = shown.full_schema_bytes - shown.schema_bytes;
  const view = (
    <div class="tools-content">
      <div class="tools-summary">
        <div>
          <strong>
            <DataText>
              {shown.active} of {shown.available} active
            </DataText>
          </strong>
          <small class="muted">
            <DataText>
              {bytes(shown.schema_bytes)} serialized schema
              {saved > 0 && ` · ${bytes(saved)} saved`}
            </DataText>
          </small>
        </div>
        <Select
          aria-label="Tool profile"
          value={shown.base_profile}
          disabled={locked}
          onChange={(event) =>
            update({ operation: "profile", profile: event.currentTarget.value })
          }
        >
          {shown.profiles.map((profile) => (
            <option value={profile} key={profile}>
              {capitalize(profile)}
            </option>
          ))}
        </Select>
      </div>
      <p class="muted tools-note">
        Changes apply between turns. Keeping a stable set improves prompt cache
        reuse.
      </p>
      {busy && (
        <p class="card tools-busy" role="status">
          Finish the current turn before changing tools.
        </p>
      )}
      {error && <LoadError error={error} />}
      <Input
        class="tools-search"
        type="search"
        value={query}
        placeholder="Find a tool…"
        aria-label="Find a tool"
        onInput={(event) => setQuery(event.currentTarget.value)}
      />
      <div class="tool-groups">
        {groups.map(([category, tools]) => (
          <section class="tool-group" key={category}>
            <h3 class="caption">
              <DataText>{labels[category] || category}</DataText>
            </h3>
            {tools.map((tool) => (
              <div
                key={tool.name}
                class={`card tool-choice ${!tool.available ? "locked" : ""}`}
              >
                <label class="tool-toggle">
                  <Input
                    type="checkbox"
                    // What was just picked, until the host answers.
                    checked={tool.active !== (saving === tool.name)}
                    disabled={locked || !tool.available}
                    onChange={(event) =>
                      update({
                        operation: "set",
                        name: tool.name,
                        active: event.currentTarget.checked,
                      })
                    }
                  />
                  <span>
                    <span class="tool-choice-head">
                      <strong>
                        <DataText>{tool.title}</DataText>
                      </strong>
                      <code>
                        <DataText>{tool.name}</DataText>
                      </code>
                      <small>
                        <DataText>{bytes(tool.schema_bytes)}</DataText>
                      </small>
                    </span>
                    <span class="tool-description">
                      <DataText>{tool.description}</DataText>
                    </span>
                    {tool.provider !== "builtin" && (
                      <small class="muted">{tool.provider}</small>
                    )}
                    {tool.reason && <small class="muted">{tool.reason}</small>}
                  </span>
                </label>
              </div>
            ))}
          </section>
        ))}
        {!groups.length && <p class="muted">No matching tools.</p>}
      </div>
    </div>
  );
  return (
    <Placeholder label="Loading tools…" when={!catalogue}>
      {view}
    </Placeholder>
  );
}
