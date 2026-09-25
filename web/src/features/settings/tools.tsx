import "./tools.css";
import { useEffect, useMemo, useState } from "preact/hooks";
import type {
  Session,
  ToolCategories,
  ToolCatalogue,
  ToolCatalogueItem,
} from "../../shared/types.ts";
import { LoadError, Select, Spinner, Input } from "../../shared/ui.tsx";
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
  const [categories, setCategories] = useState<ToolCategories>({
    categories: [],
    assignments: {},
  });
  const [query, setQuery] = useState("");
  const [saving, setSaving] = useState("");
  const [categoryName, setCategoryName] = useState("");
  const [categorySaving, setCategorySaving] = useState(false);
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
    Promise.all([
      command("tools", session, { operation: "catalog" }),
      command("tool_categories", null, { action: "list" }),
    ])
      .then(([response, categoryResponse]) => {
        if (!active) return;
        if (response.pending || categoryResponse.pending)
          throw new Error(
            "Tool settings are still loading. Try again shortly.",
          );
        setCatalogue(response.result);
        setCategories(categoryResponse.result);
      })
      .catch((failure) => {
        if (active) setError(failure);
      });
    return () => {
      active = false;
    };
  }, [session.id, session.generation]);

  const updateCategories = async (fields: {
    action: string;
    name?: string;
    category_id?: string;
  }) => {
    setCategorySaving(true);
    setError(null);
    try {
      const response = await command("tool_categories", null, fields);
      if (response.pending)
        throw new Error("Tool categories are still saving. Try again shortly.");
      setCategories(response.result);
      return true;
    } catch (failure) {
      setError(failure);
      return false;
    } finally {
      setCategorySaving(false);
    }
  };

  const categoryLabel = (id: string) =>
    labels[id] ||
    categories.categories.find((item) => item.id === id)?.name ||
    id;
  const groups = useMemo(() => {
    const found = new Map<string, ToolCatalogueItem[]>();
    const needle = query.trim().toLowerCase();
    for (const tool of catalogue?.tools || []) {
      const category =
        categories.assignments[tool.name] || tool.category || "workspace";
      if (
        needle &&
        !`${tool.name} ${tool.title} ${tool.description} ${tool.provider} ${categoryLabel(category)}`
          .toLowerCase()
          .includes(needle)
      )
        continue;
      if (!found.has(category)) found.set(category, []);
      found.get(category)!.push(tool);
    }
    return [...found].sort(([a], [b]) => {
      const position = (value: string) => {
        const builtin = categoryOrder.indexOf(value);
        if (builtin >= 0) return builtin;
        const custom = categories.categories.findIndex(
          (category) => category.id === value,
        );
        return (
          categoryOrder.length + (custom >= 0 ? custom : categoryOrder.length)
        );
      };
      return position(a) - position(b);
    });
  }, [catalogue, categories, query]);

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
      <details class="tool-categories">
        <summary>Categories</summary>
        <form
          onSubmit={(event) => {
            event.preventDefault();
            const name = categoryName.trim();
            if (!name) return;
            updateCategories({ action: "create", name }).then((saved) => {
              if (saved) setCategoryName("");
            });
          }}
        >
          <Input
            aria-label="New tool category"
            placeholder="New category"
            value={categoryName}
            disabled={!online || categorySaving}
            onInput={(event) => setCategoryName(event.currentTarget.value)}
          />
          <button disabled={!online || categorySaving || !categoryName.trim()}>
            Add
          </button>
        </form>
        {categories.categories.map((category) => (
          <form
            class="tool-category"
            key={`${category.id}:${category.name}`}
            onSubmit={(event) => {
              event.preventDefault();
              const name = new FormData(event.currentTarget)
                .get("name")
                ?.toString()
                .trim();
              if (name)
                updateCategories({
                  action: "rename",
                  category_id: category.id,
                  name,
                });
            }}
          >
            <Input
              name="name"
              aria-label={`Name for ${category.name}`}
              defaultValue={category.name}
              disabled={!online || categorySaving}
            />
            <button disabled={!online || categorySaving}>Rename</button>
            <button
              type="button"
              class="quiet"
              disabled={!online || categorySaving}
              onClick={() =>
                updateCategories({
                  action: "delete",
                  category_id: category.id,
                })
              }
            >
              Delete
            </button>
          </form>
        ))}
      </details>
      {busy && (
        <p class="tools-busy" role="status">
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
            <h3>{categoryLabel(category)}</h3>
            {tools.map((tool) => (
              <div
                key={tool.name}
                class={`tool-choice ${!tool.available ? "locked" : ""}`}
              >
                <label class="tool-toggle">
                  <Input
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
                <Select
                  aria-label={`Category for ${tool.title}`}
                  value={categories.assignments[tool.name] || ""}
                  disabled={!online || categorySaving}
                  onChange={(event) =>
                    updateCategories({
                      action: "assign",
                      name: tool.name,
                      category_id: event.currentTarget.value,
                    })
                  }
                >
                  <option value="">
                    Default · {labels[tool.category] || tool.category}
                  </option>
                  {categories.categories.map((item) => (
                    <option value={item.id} key={item.id}>
                      {item.name}
                    </option>
                  ))}
                </Select>
              </div>
            ))}
          </section>
        ))}
        {!groups.length && <p class="muted">No matching tools.</p>}
      </div>
    </div>
  );
}
