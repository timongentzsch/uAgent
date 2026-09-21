import "./tools.css";
import { useEffect, useMemo, useState } from "preact/hooks";
import type {
  Session,
  ToolCatalogue,
  ToolCatalogueItem,
} from "../../shared/types.ts";
import { LoadError, Select, Spinner } from "../../shared/ui.tsx";
import { command } from "../../state/api.ts";

const labels: Record<string, string> = {
  workspace: "Workspace",
  execute: "Execute",
  web: "Web",
  collaborate: "Collaborate",
  memory: "Memory and skills",
  system: "System",
  mcp: "MCP",
};

const categoryOrder = [
  "workspace",
  "execute",
  "web",
  "collaborate",
  "memory",
  "system",
  "mcp",
];

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
  const [catalogue, setCatalogue] = useState<ToolCatalogue | null>(null);
  const [query, setQuery] = useState("");
  const [saving, setSaving] = useState("");
  const [error, setError] = useState<unknown>(null);

  const update = async (fields: {
    operation: string;
    name?: string;
    active?: boolean;
    profile?: string;
  }) => {
    const previous = catalogue;
    if (fields.operation === "set" && fields.name && fields.active != null) {
      setCatalogue((current) => {
        if (!current) return current;
        const tools = current.tools.map((tool) =>
          tool.name === fields.name
            ? { ...tool, active: fields.active! }
            : tool,
        );
        const active = tools.filter((tool) => tool.active).length;
        const schemaBytes =
          2 +
          tools.reduce(
            (total, tool) => total + (tool.active ? tool.schema_bytes : 0),
            0,
          ) +
          Math.max(0, active - 1);
        return {
          ...current,
          profile: "custom",
          active,
          schema_bytes: schemaBytes,
          tools,
        };
      });
    }
    setError(null);
    setSaving(fields.name || fields.profile || fields.operation);
    try {
      const response = await command("tools", session, fields);
      if (response.pending)
        throw new Error("Tool settings are still loading. Try again shortly.");
      setCatalogue(response.result);
      changed();
    } catch (failure) {
      if (previous) setCatalogue(previous);
      setError(failure);
    } finally {
      setSaving("");
    }
  };

  useEffect(() => {
    let active = true;
    command("tools", session, { operation: "catalog" })
      .then((response) => {
        if (!active) return;
        if (response.pending)
          throw new Error(
            "Tool settings are still loading. Try again shortly.",
          );
        setCatalogue(response.result);
      })
      .catch((failure) => {
        if (active) setError(failure);
      });
    return () => {
      active = false;
    };
  }, [session.id, session.generation]);

  const groups = useMemo(() => {
    const found = new Map<string, ToolCatalogueItem[]>();
    const needle = query.trim().toLowerCase();
    for (const tool of catalogue?.tools || []) {
      if (
        needle &&
        !`${tool.name} ${tool.title} ${tool.description} ${tool.provider}`
          .toLowerCase()
          .includes(needle)
      )
        continue;
      const category = tool.category || "workspace";
      if (!found.has(category)) found.set(category, []);
      found.get(category)!.push(tool);
    }
    return [...found].sort(
      ([a], [b]) => categoryOrder.indexOf(a) - categoryOrder.indexOf(b),
    );
  }, [catalogue, query]);

  if (!catalogue && !error) return <Spinner label="Loading tools…" surface />;
  if (!catalogue) return <LoadError error={error} />;
  const locked = !online || busy || !!saving;
  const saved = catalogue.full_schema_bytes - catalogue.schema_bytes;
  return (
    <div class="tools-content">
      <div class="tools-summary">
        <div>
          <strong>
            {catalogue.active} of {catalogue.available} active
          </strong>
          <small class="muted">
            {catalogue.schema_bytes.toLocaleString()} serialized schema bytes
            {saved > 0 && ` · ${saved.toLocaleString()} bytes saved`}
          </small>
        </div>
        <Select
          aria-label="Tool profile"
          value={catalogue.base_profile}
          disabled={locked}
          onChange={(event) =>
            update({ operation: "profile", profile: event.currentTarget.value })
          }
        >
          {catalogue.profiles.map((profile) => (
            <option value={profile} key={profile}>
              {profile[0].toUpperCase() + profile.slice(1)}
            </option>
          ))}
        </Select>
      </div>
      <p class="muted tools-note">
        Changes apply between turns. Keeping a stable set improves prompt cache
        reuse.
      </p>
      {busy && (
        <p class="tools-busy" role="status">
          Finish the current turn before changing tools.
        </p>
      )}
      {error && <LoadError error={error} />}
      <input
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
            <h3>{labels[category] || category}</h3>
            {tools.map((tool) => (
              <label
                key={tool.name}
                class={`tool-choice ${!tool.available ? "locked" : ""}`}
              >
                <input
                  type="checkbox"
                  checked={tool.active}
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
                    <strong>{tool.title}</strong>
                    <code>{tool.name}</code>
                    <small>{tool.schema_bytes.toLocaleString()} bytes</small>
                  </span>
                  <span class="tool-description">{tool.description}</span>
                  {tool.provider !== "builtin" && (
                    <small class="muted">{tool.provider}</small>
                  )}
                  {tool.reason && <small class="muted">{tool.reason}</small>}
                </span>
              </label>
            ))}
          </section>
        ))}
        {!groups.length && <p class="muted">No matching tools.</p>}
      </div>
    </div>
  );
}
