import { useEffect, useState } from "preact/hooks";
import {
  Button,
  Cluster,
  Input,
  InputRow,
  LoadError,
  Notice,
} from "../shared/ui.tsx";
import { useAction } from "../shared/use-action.ts";
import { useResource } from "../shared/use-resource.ts";
import { useApi } from "../state/api.ts";
import "./free-start.css";

// OpenRouter sends the browser back only where it trusts the address;
// anywhere else it shows a code to paste.
const returns = () =>
  location.hostname === "localhost" || location.protocol === "https:";

// Shown while no provider is set up: signing in to OpenRouter gives this
// host a key of the person's own, kept to free models.
export function FreeStart({
  online,
  settings,
}: {
  online: boolean;
  settings: () => void;
}) {
  const { manage } = useApi();
  const { value: provider } = useResource(() => manage("provider"), []);
  // Where to approve, once started without a way back.
  const [approve, setApprove] = useState("");
  const [code, setCode] = useState("");
  const [model, setModel] = useState("");
  const { run, busy, error } = useAction();
  const finish = (code: string, state = "") =>
    run(async () => {
      const done = await manage("openrouter", {
        action: "finish",
        code,
        state,
      });
      setModel(done.model || "");
      setApprove("");
    });
  // Back from OpenRouter with the code in the address.
  // biome-ignore lint/correctness/useExhaustiveDependencies: read once, on arrival.
  useEffect(() => {
    const params = new URLSearchParams(location.search);
    const code = params.get("code");
    if (!params.has("openrouter") || !code) return;
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
  if (provider?.configured !== false && !busy && !error) return null;
  return (
    <section class="free-start" aria-label="Start with a free model">
      <strong>Start with a free model</strong>
      <p class="muted">
        Sign in with OpenRouter. Free models cost nothing and have daily limits.
      </p>
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
          <Button onClick={settings}>I have an API key</Button>
        </Cluster>
      )}
      {error ? <LoadError error={error} /> : null}
    </section>
  );
}
