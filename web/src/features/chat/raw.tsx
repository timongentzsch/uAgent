import "./raw.css";
import type { RawOptions, Exchange, JSONValue } from "../../shared/types.ts";
import { useId, useMemo, useState } from "preact/hooks";
import { useResource } from "../../shared/use-resource.ts";
import { Download } from "lucide-preact";
import { downloadRevokeMs } from "../../shared/limits.ts";
import {
  Actions,
  Button,
  Field,
  Select,
  LoadError,
  Spinner,
  Time,
} from "../../shared/ui.tsx";
import { readPages, command, routes } from "../../state/api.ts";
import { formatBody } from "../../shared/format.ts";
import { formatEventStream } from "./raw-capture.ts";

// The full body of a tool call, a context or an HTTP exchange, read from the
// host in pages. Opened from a row's menu and from the inspector.

export default function Raw({
  session,
  id,
  value,
  exchanges,
  latest,
  instructions,
  context,
  prepare,
  part = "request",
}: RawOptions & { latest?: Exchange[]; instructions?: () => void }) {
  const http = context || exchanges !== undefined;
  const [captured, setCaptured] = useState(exchanges || []);
  const [attempt, setAttempt] = useState(
    Math.max(0, (exchanges?.length || 1) - 1),
  );
  const [tab, setTab] = useState<"request" | "response">(part);
  const [source, setSource] = useState(false);
  const prefix = useId();
  const selectedExchange = captured[attempt];
  const exchange =
    latest?.find((item) => item.id === selectedExchange?.id) ||
    selectedExchange;
  // Reads again for a new target or the exchange's state, not for each new
  // exchange object; what was read for another shows nothing meanwhile.
  const target = [id, exchange?.id, exchange?.state, http && tab].join();
  const read = useResource(
    async (signal) => {
      if (value !== undefined) return;
      if (http && !exchange) {
        if (!prepare) throw new Error("This HTTP exchange was not captured.");
        const result = await command("context", prepare);
        if (signal.aborted) return;
        if (result.pending || !result.result?.exchanges?.length)
          throw new Error("Context is not available yet. Try again shortly.");
        setCaptured(result.result.exchanges);
        setAttempt(result.result.exchanges.length - 1);
        return;
      }
      const query = http
        ? `http=${encodeURIComponent(exchange!.id)}&part=${tab}`
        : `detail=${encodeURIComponent(id || "")}&raw=1`;
      const page = await readPages(routes.session(session!, query), signal);
      return {
        target,
        metadata: page.exchange,
        body: (http
          ? page.text
          : id?.startsWith("t-")
            ? JSON.parse(page.text)
            : { output: page.text }) as JSONValue,
      };
    },
    [session, id, value, exchange?.id, exchange?.state, http && tab],
  );
  const loaded = read.value?.target === target ? read.value : undefined;
  const body = value === undefined ? loaded?.body : value;
  const object =
    body && typeof body === "object" && !Array.isArray(body) ? body : undefined;
  const tool = !http && !!object && "request" in object;
  const selected = http ? body : tool ? object[tab] : (object?.output ?? body);
  const original =
    typeof selected === "string"
      ? selected
      : JSON.stringify(selected, null, 2) || "";
  const info = loaded?.metadata || exchange;
  const headers = info?.[`${tab}_headers`];
  const stream =
    http &&
    tab === "response" &&
    (/content-type:\s*text\/event-stream\b/i.test(headers || "") ||
      (!headers && /^(?:\uFEFF)?(?:data|event|id|retry):/m.test(original)));
  const text = useMemo(
    () =>
      source
        ? original
        : stream
          ? formatEventStream(original, true)
          : formatBody(original, true),
    [original, stream, source],
  );
  return (
    <div class="raw-content">
      <div class="raw-controls">
        {http && captured.length > 1 && (
          <Field label="Attempt">
            <Select
              value={attempt}
              onChange={(event) =>
                setAttempt(Number(event.currentTarget.value))
              }
            >
              {captured.map((item, index) => (
                <option key={item.id} value={index}>
                  {item.purpose || "Model call"} · attempt{" "}
                  {item.attempt || index + 1} · {item.status || item.state}
                </option>
              ))}
            </Select>
          </Field>
        )}
        {info && (
          <p class="muted small">
            {info.preview
              ? "Current context · not sent yet"
              : `${info.method} ${info.url} · ${info.status || info.state}`}{" "}
            <Time value={info.time} />
          </p>
        )}
        {(http || tool) && (
          <div
            class="tabs"
            role="tablist"
            aria-label="Exchange"
            onKeyDown={(event) => {
              if (
                !["ArrowLeft", "ArrowRight"].includes(event.key) ||
                info?.preview
              )
                return;
              event.preventDefault();
              const next = tab === "request" ? "response" : "request";
              setTab(next);
              document.getElementById(`${prefix}-${next}`)?.focus();
            }}
          >
            {(["request", "response"] as const).map((name) => (
              <Button
                type="button"
                id={`${prefix}-${name}`}
                role="tab"
                aria-controls={`${prefix}-body`}
                tabIndex={tab === name ? 0 : -1}
                aria-selected={tab === name}
                disabled={name === "response" && info?.preview}
                key={name}
                onClick={() => setTab(name)}
              >
                {name === "request" ? "Request" : "Response"}
              </Button>
            ))}
          </div>
        )}
        {info?.state === "receiving" && tab === "response" && (
          <p class="muted small">Receiving · captured so far</p>
        )}
        {(object?.complete === false ||
          info?.complete === false ||
          info?.state === "interrupted") && (
          <p class="muted small">This capture is incomplete.</p>
        )}
      </div>
      {context && instructions && (
        <Button onClick={instructions}>Instructions</Button>
      )}
      <div
        class="raw-body"
        id={`${prefix}-body`}
        role={http || tool ? "tabpanel" : undefined}
        aria-labelledby={http || tool ? `${prefix}-${tab}` : undefined}
        tabIndex={0}
      >
        {headers && (
          <details>
            <summary>HTTP headers · credentials redacted</summary>
            <pre>{headers}</pre>
          </details>
        )}
        {read.error ? (
          <LoadError error={read.error} retry={read.retry} />
        ) : body === undefined ? (
          <Spinner label="Loading full body…" surface />
        ) : !text.trim() ? (
          <p class="muted">No content recorded.</p>
        ) : (
          <pre>{text}</pre>
        )}
      </div>
      <Actions>
        <div class="tabs" role="group" aria-label="Body display">
          {([false, true] as const).map((raw) => (
            <Button
              type="button"
              key={String(raw)}
              disabled={body === undefined}
              aria-pressed={source === raw}
              onClick={() => setSource(raw)}
            >
              {raw ? "Source" : stream ? "Events" : "Readable"}
            </Button>
          ))}
        </div>
        <Button
          type="button"
          disabled={body === undefined}
          onClick={() => {
            const url = URL.createObjectURL(
              new Blob([original], { type: "text/plain;charset=utf-8" }),
            );
            const link = document.createElement("a");
            link.href = url;
            link.download = `${exchange?.id || id || "result"}-${tab}.txt`;
            link.click();
            // Some browsers start the download after click() returns.
            setTimeout(() => URL.revokeObjectURL(url), downloadRevokeMs);
          }}
        >
          <Download />
          Download
        </Button>
      </Actions>
    </div>
  );
}
