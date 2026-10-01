import "./tools.css";
import { useMemo, useState } from "preact/hooks";
import type {
  Session,
  ToolCategories,
  ToolCatalogue,
  ToolCatalogueItem,
} from "../../shared/types.ts";
import {
  Button,
  DataText,
  Input,
  LoadError,
  Placeholder,
  Select,
} from "../../shared/ui.tsx";
import { command } from "../../state/api.ts";
import { useAction } from "../../shared/use-action.ts";
import { useResource } from "../../shared/use-resource.ts";

const labels: Record<string, string> = {
  workspace: "Workspace",
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
  const [categories, setCategories] = useState<ToolCategories>({
    categories: [],
    assignments: {},
  });
  const {
    value: catalogue,
    error: toolError,
    setValue: setCatalogue,
    setError,
  } = useResource<ToolCatalogue>(async () => {
    const [response, categoryResponse] = await Promise.all([
      command("tools", session, { operation: "catalog" }),
      command("tool_categories", null, { action: "list" }),
    ]);
    if (response.pending || categoryResponse.pending)
      throw new Error("Tool settings are still loading. Try again shortly.");
    // The categories are the host's, the same for every session.
    setCategories(categoryResponse.result);
    return response.result;
  }, [session.id, session.generation]);
  const [query, setQuery] = useState("");
  const [saving, setSaving] = useState("");
  const [categoryName, setCategoryName] = useState("");
  const {
    run: saveCategories,
    busy: categorySaving,
    error: categoryError,
  } = useAction();
  const error = toolError ?? categoryError;

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

  const updateCategories = (fields: {
    action: string;
    name?: string;
    category_id?: string;
  }) =>
    saveCategories(async () => {
      const response = await command("tool_categories", null, fields);
      if (response.pending)
        throw new Error("Tool categories are still saving. Try again shortly.");
      setCategories(response.result);
    });

  const categoryLabel = (id: string) =>
    labels[id] ||
    categories.categories.find((item) => item.id === id)?.name ||
    id;
  const groups = useMemo(() => {
    const found = new Map<string, ToolCatalogueItem[]>();
    const needle = query.trim().toLowerCase();
    for (const tool of (catalogue ?? SAMPLE).tools) {
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
              {shown.schema_bytes.toLocaleString()} serialized schema bytes
              {saved > 0 && ` · ${saved.toLocaleString()} bytes saved`}
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
          <Button
            type="submit"
            disabled={!online || categorySaving || !categoryName.trim()}
          >
            Add
          </Button>
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
            <Button type="submit" disabled={!online || categorySaving}>
              Rename
            </Button>
            <Button
              type="button"
              variant="quiet"
              disabled={!online || categorySaving}
              onClick={() =>
                updateCategories({
                  action: "delete",
                  category_id: category.id,
                })
              }
            >
              Delete
            </Button>
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
            <h3>
              <DataText>{categoryLabel(category)}</DataText>
            </h3>
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
                      <strong>
                        <DataText>{tool.title}</DataText>
                      </strong>
                      <code>
                        <DataText>{tool.name}</DataText>
                      </code>
                      <small>
                        <DataText>
                          {tool.schema_bytes.toLocaleString()} bytes
                        </DataText>
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
                {categories.categories.length > 0 && (
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
                )}
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
