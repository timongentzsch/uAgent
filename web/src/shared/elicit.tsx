import type { Elicitation } from "./types.ts";
import { useState } from "preact/hooks";
import {
  Actions,
  Button,
  cleanText,
  Field,
  Input,
  Notice,
  Row,
  Select,
  Switch,
} from "./ui.tsx";

type Value = string | number | boolean | string[] | undefined;

const INPUT_TYPES: Record<string, "email" | "url" | "date"> = {
  email: "email",
  uri: "url",
  date: "date",
};

export default function Elicit({
  request,
  idle,
  send,
  cancel,
}: {
  request: Elicitation;
  idle: boolean;
  send: (text: string) => void;
  cancel: () => void;
}) {
  const form = request.mode === "form";
  const fields = Object.entries(request.requestedSchema?.properties ?? {});
  const required = new Set(request.requestedSchema?.required ?? []);
  const [values, setValues] = useState<Record<string, Value>>(() =>
    Object.fromEntries(fields.map(([name, field]) => [name, field.default])),
  );
  const set = (name: string, value: Value) =>
    setValues((current) => ({ ...current, [name]: value }));
  // Only what the person filled in: an untouched optional field is left out.
  const content = () =>
    Object.fromEntries(
      fields.flatMap(([name, field]) => {
        const value = values[name];
        if (value === undefined || value === "") return [];
        if (Array.isArray(value) && !value.length && !required.has(name))
          return [];
        const number = field.type === "number" || field.type === "integer";
        return [[name, number ? Number(value) : value]];
      }),
    );
  const answer = (action: "accept" | "decline") =>
    send(
      JSON.stringify(
        action === "accept" && form
          ? { action, content: content() }
          : { action },
      ),
    );
  const url = request.url ?? "";
  const host = request.host ?? "";
  const at = url.toLowerCase().indexOf(host, url.indexOf("://") + 3);
  return (
    <form
      class="elicit"
      onSubmit={(event) => {
        event.preventDefault();
        answer("accept");
      }}
    >
      <div class="ask-questions elicit-fields">
        <p>
          <strong>{cleanText(request.server)}</strong> asks:{" "}
          {cleanText(request.message)}
        </p>
        {form ? (
          fields.map(([name, field]) => {
            const label = cleanText(field.title || name);
            const title = required.has(name) ? label : `${label} (optional)`;
            const help = field.description && cleanText(field.description);
            const value = values[name];
            const chosen = (value as string[] | undefined) ?? [];
            if (field.type === "boolean")
              return (
                <Row key={name} flush label={title} detail={help}>
                  <Switch
                    label={label}
                    checked={!!value}
                    onChange={(checked) => set(name, checked)}
                  />
                </Row>
              );
            if (field.type === "array")
              return (
                <div class="field" role="group" aria-label={title} key={name}>
                  <span class="field-label">{title}</span>
                  <div class="ask-options">
                    {field.choices.map((option) => (
                      <label class="card entry ask-option" key={option.value}>
                        <Input
                          type="checkbox"
                          checked={chosen.includes(option.value)}
                          onChange={(event) => {
                            const on = event.currentTarget.checked;
                            set(
                              name,
                              field.choices
                                .map((each) => each.value)
                                .filter((each) =>
                                  each === option.value
                                    ? on
                                    : chosen.includes(each),
                                ),
                            );
                          }}
                        />
                        <span>
                          <strong>{cleanText(option.label)}</strong>
                        </span>
                      </label>
                    ))}
                  </div>
                  {help && <small class="muted">{help}</small>}
                </div>
              );
            return (
              <Field key={name} label={title} help={help}>
                {field.choices.length ? (
                  <Select
                    required={required.has(name)}
                    value={(value as string) ?? ""}
                    onChange={(event) => set(name, event.currentTarget.value)}
                  >
                    {value === undefined && <option value="">Choose…</option>}
                    {field.choices.map((option) => (
                      <option key={option.value} value={option.value}>
                        {cleanText(option.label)}
                      </option>
                    ))}
                  </Select>
                ) : (
                  <Input
                    type={
                      field.type === "string"
                        ? (INPUT_TYPES[field.format ?? ""] ?? "text")
                        : "number"
                    }
                    step={field.type === "integer" ? 1 : "any"}
                    min={field.minimum}
                    max={field.maximum}
                    minLength={field.minLength}
                    maxLength={field.maxLength}
                    required={required.has(name)}
                    autoComplete="off"
                    value={value === undefined ? "" : String(value)}
                    onInput={(event) => set(name, event.currentTarget.value)}
                  />
                )}
              </Field>
            );
          })
        ) : (
          <>
            {/* Shown whole and never fetched; the person opens it. */}
            <pre class="plain elicit-url">
              {url.slice(0, at)}
              <strong>{url.slice(at, at + host.length)}</strong>
              {url.slice(at + host.length)}
            </pre>
            {request.punycode && (
              <Notice tone="warning">
                The host is an internationalised (punycode) name: check it is
                the site you expect.
              </Notice>
            )}
            <a href={url} target="_blank" rel="noopener noreferrer">
              Open {host} in your browser
            </a>
          </>
        )}
      </div>
      <Actions>
        <Button onClick={cancel} disabled={idle}>
          Cancel
        </Button>
        <Button onClick={() => answer("decline")} disabled={idle}>
          Decline
        </Button>
        <Button type="submit" variant="primary" disabled={idle}>
          {form ? "Accept" : "Continue"}
        </Button>
      </Actions>
    </form>
  );
}
