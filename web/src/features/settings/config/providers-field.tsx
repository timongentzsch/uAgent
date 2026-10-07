import { useState } from "preact/hooks";
import type { JSONValue } from "../../../shared/types.ts";
import { Button, Input, Row, ValueSelect } from "../../../shared/ui.tsx";

// What the host shows in place of a key it holds, and takes back as "keep".
const HIDDEN = "<redacted>";

type Provider = Record<string, JSONValue>;
type Providers = Record<string, Provider>;

const WIRES: [string, string][] = [
  ["chat_completions", "Chat completions (OpenAI-compatible)"],
  ["responses", "Responses (OpenAI)"],
  ["anthropic_messages", "Messages (Anthropic)"],
];

// The fields edited here; anything else a provider holds is kept as written
// and named under it.
const EDITED = ["base_url", "api_key", "wire_api"];

// The named providers, one block each: where its requests go, the key sent
// with them and the API it speaks. A model is then chosen as provider/model.
// Every change is saved as it is made; a key that was not retyped is kept.
export function ProvidersField({
  value,
  disabled,
  save,
}: {
  // What this scope holds, keys hidden; absent when it holds none.
  value?: JSONValue;
  disabled: boolean;
  // Saves the whole set, or removes the setting when it is empty.
  save: (text: string) => Promise<boolean>;
}) {
  const held: Providers =
    value && typeof value === "object" && !Array.isArray(value)
      ? (value as Providers)
      : {};
  const [name, setName] = useState("");
  const [address, setAddress] = useState("");
  const put = (next: Providers) =>
    save(Object.keys(next).length ? JSON.stringify(next) : "");
  const change = (provider: string, field: string, text: string) => {
    const entry = { ...held[provider] };
    if (text) entry[field] = text;
    else delete entry[field];
    return put({ ...held, [provider]: entry });
  };
  // Saved on leaving the field or Enter, and only when it differs.
  const field = (provider: string, key: string, label: string) => ({
    "aria-label": `${provider} ${label}`,
    disabled,
    onBlur: (event: { currentTarget: HTMLInputElement }) => {
      const text = event.currentTarget.value.trim();
      const before = held[provider][key];
      if (
        text !== (typeof before === "string" && before !== HIDDEN ? before : "")
      )
        void change(provider, key, text);
    },
    onKeyDown: (event: KeyboardEvent & { currentTarget: HTMLInputElement }) => {
      if (event.key === "Enter") event.currentTarget.blur();
    },
  });
  const wanted = name.trim();
  const taken =
    !/^[A-Za-z0-9_-]+$/.test(wanted) || wanted === "all" || wanted in held;
  return (
    <div class="providers">
      {Object.entries(held).map(([provider, entry]) => {
        const key = typeof entry.api_key === "string" ? entry.api_key : "";
        const other = Object.keys(entry).filter((k) => !EDITED.includes(k));
        return (
          <section key={provider} class="provider" aria-label={provider}>
            <Row
              label={<strong>{provider}</strong>}
              detail={`Choose its models as ${provider}/model`}
            >
              <Button
                variant="quiet"
                size="compact"
                disabled={disabled}
                onClick={() => {
                  const { [provider]: _, ...rest } = held;
                  void put(rest);
                }}
              >
                Remove
              </Button>
            </Row>
            <Row label="Address" detail="Where its requests go">
              <Input
                {...field(provider, "base_url", "address")}
                type="url"
                placeholder="https://…/v1"
                // Typed freely and saved on leaving: what is saved replaces
                // the field, nothing else does.
                key={String(entry.base_url)}
                defaultValue={
                  typeof entry.base_url === "string" ? entry.base_url : ""
                }
              />
            </Row>
            <Row
              label="Key"
              detail={
                key.startsWith("$")
                  ? `Taken from the variable ${key.slice(1)}`
                  : "Sent with each request; $NAME takes it from a variable"
              }
            >
              <Input
                {...field(provider, "api_key", "key")}
                type={key.startsWith("$") ? "text" : "password"}
                placeholder={
                  key === HIDDEN ? "Set · enter a replacement" : "Not set"
                }
                key={key}
                defaultValue={key === HIDDEN ? "" : key}
              />
            </Row>
            <Row label="Speaks" detail="The API this endpoint implements">
              <ValueSelect
                aria-label={`${provider} API`}
                disabled={disabled}
                value={
                  typeof entry.wire_api === "string"
                    ? entry.wire_api
                    : "chat_completions"
                }
                onChange={(event) =>
                  void change(provider, "wire_api", event.currentTarget.value)
                }
              >
                {WIRES.map(([id, label]) => (
                  <option key={id} value={id}>
                    {label}
                  </option>
                ))}
              </ValueSelect>
            </Row>
            {other.length > 0 && (
              <p class="group-footer">
                Also set in the file and kept as written: {other.join(", ")}.
              </p>
            )}
          </section>
        );
      })}
      <Row
        label="Add a provider"
        detail="A short name (letters, digits, - and _) and its address"
      >
        <Input
          aria-label="New provider name"
          placeholder="name"
          disabled={disabled}
          value={name}
          onInput={(event) => setName(event.currentTarget.value)}
        />
        <Input
          aria-label="New provider address"
          type="url"
          placeholder="https://…/v1"
          disabled={disabled}
          value={address}
          onInput={(event) => setAddress(event.currentTarget.value)}
        />
        <Button
          size="compact"
          disabled={
            disabled || taken || !/^https?:\/\/\S+$/.test(address.trim())
          }
          onClick={async () => {
            if (
              await put({ ...held, [wanted]: { base_url: address.trim() } })
            ) {
              setName("");
              setAddress("");
            }
          }}
        >
          Add
        </Button>
      </Row>
    </div>
  );
}
