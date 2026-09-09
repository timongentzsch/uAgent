import "./raw.css";
import type { RawOptions, Exchange, JSONValue } from "./types.ts";
import { useEffect, useId, useMemo, useState } from "preact/hooks";
import { Copy, Download } from "lucide-preact";
import { Field, Select, Skeleton, LoadError, copyText } from "./ui.tsx";
import { readPages, command } from "./store.ts";
import { formatBody } from "./format.ts";
import { formatEventStream } from "./event-stream.ts";

export default function Raw({
  session,
  id,
  value,
  exchanges,
  latest,
  prompt,
  context,
  prepare,
  part = "request",
}: RawOptions & { latest?: Exchange[]; prompt?: () => void }) {
  const http = context || exchanges !== undefined;
  const [captured, setCaptured] = useState(exchanges || []);
  const [attempt, setAttempt] = useState(
    Math.max(0, (exchanges?.length || 1) - 1),
  );
  const [body, setBody] = useState<JSONValue | undefined>(value);
  const [tab, setTab] = useState<"request" | "response">(part);
  const [source, setSource] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const [metadata, setMetadata] = useState<Exchange>();
  const prefix = useId();
  const selectedExchange = captured[attempt];
  const exchange =
    latest?.find((item) => item.id === selectedExchange?.id) ||
    selectedExchange;
  useEffect(() => {
    if (value !== undefined) return;
    const abort = new AbortController();
    setBody(undefined);
    setMetadata(undefined);
    setError(null);
    const read = async () => {
      if (http && !exchange) {
        if (!prepare) throw new Error("This HTTP exchange was not captured.");
        const result = await command("context", prepare);
        if (abort.signal.aborted) return;
        if (result.pending || !result.result?.exchanges?.length)
          throw new Error("Context is not available yet. Try again shortly.");
        setCaptured(result.result.exchanges);
        setAttempt(result.result.exchanges.length - 1);
        return;
      }
      const query = http
        ? `http=${encodeURIComponent(exchange!.id)}&part=${tab}`
        : `detail=${encodeURIComponent(id || "")}&raw=1`;
      const page = await readPages(
        `/api/sessions/${session}?${query}`,
        abort.signal,
      );
      if (abort.signal.aborted) return;
      setMetadata(page.exchange);
      setBody(
        http
          ? page.text
          : id?.startsWith("t-")
            ? JSON.parse(page.text)
            : { output: page.text },
      );
    };
    read().catch((failure) => {
      if (!abort.signal.aborted) setError(failure);
    });
    return () => abort.abort();
  }, [session, id, value, exchange?.id, exchange?.state, http && tab, retry]);
  const object =
    body && typeof body === "object" && !Array.isArray(body) ? body : undefined;
  const tool = !http && !!object && "request" in object;
  const selected = http ? body : tool ? object[tab] : (object?.output ?? body);
  const original =
    typeof selected === "string"
      ? selected
      : JSON.stringify(selected, null, 2) || "";
  const info = metadata || exchange;
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
            {info.time && <time>{new Date(info.time).toLocaleString()}</time>}
          </p>
        )}
        {(http || tool) && (
          <div
            class="raw-tabs"
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
              <button
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
              </button>
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
      {context && prompt && <button onClick={prompt}>System prompt</button>}
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
        {error ? (
          <LoadError error={error} retry={() => setRetry(retry + 1)} />
        ) : body === undefined ? (
          <Skeleton rows={12} label="Loading full body…" />
        ) : (
          <pre>{text}</pre>
        )}
      </div>
      <footer class="dialog-actions">
        <div class="raw-tabs" role="group" aria-label="Body display">
          {([false, true] as const).map((raw) => (
            <button
              type="button"
              key={String(raw)}
              disabled={body === undefined}
              aria-pressed={source === raw}
              onClick={() => setSource(raw)}
            >
              {raw ? "Source" : stream ? "Events" : "Readable"}
            </button>
          ))}
        </div>
        <button
          type="button"
          class="with-icon"
          disabled={body === undefined}
          onClick={() => copyText(original).catch(setError)}
        >
          <Copy />
          Copy raw body
        </button>
        <button
          type="button"
          class="with-icon"
          disabled={body === undefined}
          onClick={() => {
            const url = URL.createObjectURL(
              new Blob([original], { type: "text/plain;charset=utf-8" }),
            );
            const link = document.createElement("a");
            link.href = url;
            link.download = `${exchange?.id || id || "result"}-${tab}.txt`;
            link.click();
            setTimeout(() => URL.revokeObjectURL(url), 0);
          }}
        >
          <Download />
          Download
        </button>
      </footer>
    </div>
  );
}
