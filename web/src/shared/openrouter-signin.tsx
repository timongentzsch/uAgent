import type { ComponentChildren } from "preact";
import { useEffect, useState } from "preact/hooks";
import { useApi } from "../state/api.ts";
import { Button, Cluster, Input, InputRow, LoadError, Notice } from "./ui.tsx";
import { useAction } from "./use-action.ts";

// OpenRouter sends the browser back only where it trusts the address;
// anywhere else it shows a code to paste.
const returns = () =>
  location.hostname === "localhost" || location.protocol === "https:";

// Signing in to OpenRouter gives this host a key of the person's own, kept
// to free models. `arrival` is where a return from OpenRouter is finished:
// one place, since the address carries the code once.
export function OpenRouterSignIn({
  online,
  arrival = false,
  done,
  children,
}: {
  online: boolean;
  arrival?: boolean;
  // Called with the model new conversations now use.
  done?: (model: string) => void;
  // What else can be done from here, beside the button.
  children?: ComponentChildren;
}) {
  const { manage } = useApi();
  // Where to approve, once started without a way back.
  const [approve, setApprove] = useState("");
  const [code, setCode] = useState("");
  const [model, setModel] = useState("");
  const { run, busy, error } = useAction();
  const finish = (code: string, state = "") =>
    run(async () => {
      const signed = await manage("openrouter", {
        action: "finish",
        code,
        state,
      });
      setModel(signed.model || "");
      setApprove("");
      done?.(signed.model || "");
    });
  // biome-ignore lint/correctness/useExhaustiveDependencies: read once, on arrival.
  useEffect(() => {
    const params = new URLSearchParams(location.search);
    const code = params.get("code");
    if (!arrival || !params.has("openrouter") || !code) return;
    history.replaceState(
      history.state,
      "",
      `${location.pathname}${location.hash}`,
    );
    void finish(code, params.get("state") || "");
  }, []);
  const begin = () =>
    run(async () => {
      const callback = returns()
        ? `${location.origin}${location.pathname}?openrouter=1`
        : "";
      const { url } = await manage("openrouter", { action: "start", callback });
      if (callback) location.assign(url);
      else setApprove(url);
    });
  if (model)
    return (
      <Notice tone="info">
        Signed in to OpenRouter. New conversations use {model}.
      </Notice>
    );
  return (
    <>
      {approve ? (
        <InputRow
          as="form"
          onSubmit={(event) => {
            event.preventDefault();
            void finish(code);
          }}
        >
          <a href={approve} target="_blank" rel="noreferrer">
            Open OpenRouter and approve
          </a>
          <Input
            aria-label="Code from OpenRouter"
            placeholder="Paste the code from OpenRouter"
            autocomplete="off"
            value={code}
            onInput={(event) => setCode(event.currentTarget.value)}
          />
          <Button
            type="submit"
            variant="primary"
            busy={busy}
            disabled={!online || !code.trim()}
          >
            Finish sign-in
          </Button>
        </InputRow>
      ) : (
        <Cluster>
          <Button
            variant="primary"
            busy={busy}
            disabled={!online}
            onClick={begin}
          >
            Sign in with OpenRouter
          </Button>
          {children}
        </Cluster>
      )}
      {error ? <LoadError error={error} /> : null}
    </>
  );
}
