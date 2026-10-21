import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import { formatJSON } from "../../../shared/format.ts";
import type { JSONValue } from "../../../shared/types.ts";
import {
  Actions,
  Button,
  Field,
  Group,
  Input,
  Modal,
  Row,
  Select,
  Switch,
  Textarea,
  useDialogClose,
} from "../../../shared/ui.tsx";
import { REDACTED } from "./facts.ts";

// What the host shows in place of a key it holds, and takes back as "keep".
const HIDDEN = "<redacted>";

type Provider = Record<string, JSONValue>;
type Providers = Record<string, Provider>;

// Each API: its id, its short name in the list, its name to choose it by.
const WIRES: [string, string, string][] = [
  [
    "chat_completions",
    "Chat completions",
    "Chat completions (OpenAI-compatible)",
  ],
  ["responses", "Responses", "Responses (OpenAI)"],
  ["anthropic_messages", "Anthropic", "Messages (Anthropic)"],
];

// The fields edited here; anything else a provider holds is kept as written
// and named in its sheet.
const EDITED = ["base_url", "api_key", "wire_api", "hosted_tools"];

const text = (value?: JSONValue) => (typeof value === "string" ? value : "");
const wireOf = (entry: Provider) => text(entry.wire_api) || "chat_completions";
const isObject = (value: JSONValue): value is Provider =>
  !!value && typeof value === "object" && !Array.isArray(value);

// The named providers as a list of their own: one row each, edited in a
// sheet, and Add in the caption. A model is then chosen as provider/model.
export function ProvidersField({
  label,
  footer,
  value,
  error,
  disabled,
  save,
}: {
  label: string;
  // What the setting is for, who decides instead, or why a save failed.
  footer: ComponentChildren;
  // What this scope holds, keys hidden; absent when it holds none.
  value?: JSONValue;
  // Why its last save was refused.
  error?: string;
  disabled: boolean;
  // Saves the whole set, or removes the setting when it is empty.
  save: (text: string) => Promise<boolean>;
}) {
  const held: Providers = value && isObject(value) ? (value as Providers) : {};
  // The provider whose sheet is open; "" while adding one.
  const [open, setOpen] = useState<string>();
  return (
    <>
      <Group
        title={label}
        footer={footer}
        action={
          <Button
            size="compact"
            disabled={disabled}
            onClick={() => setOpen("")}
          >
            Add a provider
          </Button>
        }
      >
        {Object.entries(held).map(([name, entry]) => (
          <Row
            key={name}
            label={name}
            detail={text(entry.base_url) || "No address"}
          >
            {WIRES.find(([id]) => id === wireOf(entry))?.[1]}
            <Button
              size="compact"
              aria-label={`Edit ${name}`}
              disabled={disabled}
              onClick={() => setOpen(name)}
            >
              Edit
            </Button>
          </Row>
        ))}
      </Group>
      {open !== undefined && (
        <Modal
          title={open ? "Edit provider" : "Add a provider"}
          layout="sheet"
          close={() => setOpen(undefined)}
        >
          <ProviderForm
            name={open}
            held={held}
            error={error}
            disabled={disabled}
            save={(next) =>
              save(Object.keys(next).length ? JSON.stringify(next) : "")
            }
          />
        </Modal>
      )}
    </>
  );
}

// One provider, saved as a whole with the rest unchanged and in their
// order. A key left empty keeps the one saved. A hosted tool is defined as
// the endpoint's API documents it and sent as written.
function ProviderForm({
  name: before,
  held,
  error,
  disabled,
  save,
}: {
  // Empty for a new provider.
  name: string;
  held: Providers;
  error?: string;
  disabled: boolean;
  save: (next: Providers) => Promise<boolean>;
}) {
  const close = useDialogClose();
  const entry = held[before] || {};
  const stored = text(entry.api_key);
  const listed = Array.isArray(entry.hosted_tools) ? entry.hosted_tools : [];
  const [name, setName] = useState(before);
  const [address, setAddress] = useState(text(entry.base_url));
  const [key, setKey] = useState(stored === HIDDEN ? "" : stored);
  const [wire, setWire] = useState(wireOf(entry));
  const [search, setSearch] = useState(listed.includes("web_search"));
  const [tools, setTools] = useState(listed.filter(isObject));
  // The definition open for editing, as typed, and why it was refused.
  const [definition, setDefinition] = useState<{
    index: number;
    text: string;
    problem?: string;
  }>();
  const [busy, setBusy] = useState(false);
  const [refused, setRefused] = useState(false);
  const wanted = name.trim();
  const valid =
    /^[A-Za-z0-9_-]+$/.test(wanted) &&
    wanted !== "all" &&
    (wanted === before || !(wanted in held)) &&
    /^https?:\/\/\S+$/.test(address.trim());
  const other = Object.keys(entry).filter((field) => !EDITED.includes(field));
  const edit = (index: number) =>
    setDefinition({
      index,
      text: tools[index] ? formatJSON(JSON.stringify(tools[index])) : "",
    });
  // The open definition taken into the list, or refused and left open.
  const take = () => {
    if (!definition) return tools;
    let tool: JSONValue = null;
    try {
      tool = JSON.parse(definition.text);
    } catch {}
    if (!isObject(tool) || typeof tool.type !== "string") {
      setDefinition({
        ...definition,
        problem: 'A definition is a JSON object with a "type".',
      });
      return;
    }
    const next = tools.toSpliced(definition.index, 1, tool);
    setTools(next);
    setDefinition(undefined);
    return next;
  };
  const commit = async (next: Providers) => {
    setBusy(true);
    const saved = await save(next);
    setBusy(false);
    setRefused(!saved);
    if (saved) close();
  };
  const submit = (event: Event) => {
    event.preventDefault();
    const defined = take();
    if (!defined || !valid) return;
    const next: Provider = {
      ...entry,
      base_url: address.trim(),
      wire_api: wire,
    };
    if (key.trim()) next.api_key = key.trim();
    else if (stored !== HIDDEN) delete next.api_key;
    const hosted = [...(search ? ["web_search"] : []), ...defined];
    if (hosted.length) next.hosted_tools = hosted;
    else delete next.hosted_tools;
    // The host carries a hidden key over to the new name, then drops this.
    if (before && wanted !== before) next.renamed_from = before;
    const entries = Object.entries(held).map(([provider, value]) =>
      provider === before ? [wanted, next] : [provider, value],
    );
    void commit(
      Object.fromEntries(before ? entries : [...entries, [wanted, next]]),
    );
  };
  const editor = definition && (
    <div key="definition" class="group-block provider-tool">
      <Textarea
        aria-label="Definition"
        grow
        spellcheck={false}
        value={definition.text}
        onInput={(event) =>
          setDefinition({
            index: definition.index,
            text: event.currentTarget.value,
          })
        }
      />
      {definition.problem && (
        <small class="setting-error" role="alert">
          {definition.problem}
        </small>
      )}
      <Actions>
        <Button
          onClick={() => {
            setTools(tools.filter((_, index) => index !== definition.index));
            setDefinition(undefined);
          }}
        >
          Remove
        </Button>
        <Button onClick={take}>Done</Button>
      </Actions>
    </div>
  );
  return (
    <form class="provider-form" onSubmit={submit}>
      <Field
        label="Name"
        help={`Letters, digits, - and _. Its models are chosen as ${wanted || "name"}/model.`}
      >
        <Input
          value={name}
          spellcheck={false}
          autocomplete="off"
          onInput={(event) => setName(event.currentTarget.value)}
        />
      </Field>
      <Field label="Address" help="Where its requests go">
        <Input
          type="url"
          placeholder="https://…/v1"
          value={address}
          onInput={(event) => setAddress(event.currentTarget.value)}
        />
      </Field>
      <Field
        label="Key"
        help={
          key.startsWith("$")
            ? `Taken from the variable ${key.slice(1)}`
            : "Sent with each request; $NAME takes it from a variable"
        }
      >
        <Input
          type={key.startsWith("$") ? "text" : "password"}
          placeholder={stored === HIDDEN ? REDACTED : "Not set"}
          autocomplete="off"
          value={key}
          onInput={(event) => setKey(event.currentTarget.value)}
        />
      </Field>
      <Field label="Speaks">
        <Select
          value={wire}
          onChange={(event) => setWire(event.currentTarget.value)}
        >
          {WIRES.map(([id, , full]) => (
            <option key={id} value={id}>
              {full}
            </option>
          ))}
        </Select>
      </Field>
      <Group
        title="Hosted tools"
        action={
          <Button
            size="compact"
            disabled={!!definition}
            onClick={() => edit(tools.length)}
          >
            Define a hosted tool
          </Button>
        }
        footer="Tools this endpoint runs itself, defined as its API documents them. Sent with every request to this provider."
      >
        <Row label="Web search" detail="Replaces uAgent's own search">
          <Switch label="Web search" checked={search} onChange={setSearch} />
        </Row>
        {tools.map((tool, index) =>
          index === definition?.index ? (
            editor
          ) : (
            <Row
              key={index}
              label={text(tool.name) || text(tool.type)}
              detail={text(tool.type)}
            >
              <Button
                size="compact"
                aria-label={`Edit ${text(tool.name) || text(tool.type)}`}
                disabled={!!definition}
                onClick={() => edit(index)}
              >
                Edit
              </Button>
            </Row>
          ),
        )}
        {definition?.index === tools.length && editor}
      </Group>
      {other.length > 0 && (
        <small class="muted">Also in the file: {other.join(", ")}</small>
      )}
      {refused && error && (
        <p class="setting-error" role="alert">
          {error}
        </p>
      )}
      <Actions>
        {before && (
          <Button
            variant="destructive"
            disabled={disabled || busy}
            onClick={() => {
              const { [before]: _, ...rest } = held;
              void commit(rest);
            }}
          >
            Remove
          </Button>
        )}
        <Button onClick={close}>Cancel</Button>
        <Button
          type="submit"
          variant="primary"
          busy={busy}
          disabled={disabled || !valid}
        >
          Save
        </Button>
      </Actions>
    </form>
  );
}
