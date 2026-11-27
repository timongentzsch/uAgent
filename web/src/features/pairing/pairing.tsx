import { useAction } from "../../shared/use-action.ts";
import { useEffect, useState } from "preact/hooks";
import { api, routes } from "../../state/api.ts";
import { failure } from "../../shared/types.ts";
import { Button, Mark, Input, Notice } from "../../shared/ui.tsx";
import "./pairing.css";

// The code a pairing link carries (`uagent --web` prints <origin>/#pair=…);
// it leaves the address at once, so it never lands in history.
function linkedCode() {
  const code = new URLSearchParams(location.hash.slice(1)).get("pair");
  if (code !== null)
    history.replaceState(
      history.state,
      "",
      `${location.pathname}${location.search}`,
    );
  return code?.trim() || "";
}

export default function Pairing({
  paired,
  report,
}: {
  paired: () => Promise<void>;
  report: (error: unknown) => void;
}) {
  const [code, setCode] = useState("");
  const [online, setOnline] = useState(navigator.onLine);
  useEffect(() => {
    const changed = () => setOnline(navigator.onLine);
    addEventListener("online", changed);
    addEventListener("offline", changed);
    return () => {
      removeEventListener("online", changed);
      removeEventListener("offline", changed);
    };
  }, []);
  // The host refused the code: used, mistyped or too old.
  const [rejected, setRejected] = useState(false);
  const { run, busy } = useAction({
    report: (error) => {
      if (failure(error).status === 403) setRejected(true);
      else report(error);
    },
  });
  const pair = (value: string) =>
    run(async () => {
      setRejected(false);
      await api(routes.auth, {
        code: value.trim(),
        name: /iPhone|iPad/.test(navigator.userAgent)
          ? "iOS device"
          : /Android/.test(navigator.userAgent)
            ? "Android device"
            : "Browser",
      });
      await paired();
    });
  // A link opened here, or pasted into this tab later: only its hash
  // changes then.
  // biome-ignore lint/correctness/useExhaustiveDependencies: listens once; the first render's pair is enough
  useEffect(() => {
    const follow = () => {
      const linked = linkedCode();
      if (!linked) return;
      setCode(linked);
      void pair(linked);
    };
    follow();
    addEventListener("hashchange", follow);
    return () => removeEventListener("hashchange", follow);
  }, []);
  return (
    <main class="pairing">
      <Mark className="wordmark" />
      <h1>Connect device</h1>
      <p>
        Connect this device to the µAgent host. Run <code>uagent --web</code> on
        that computer for a fresh pairing code, or open the link it prints.
      </p>
      <form
        onSubmit={(event) => {
          event.preventDefault();
          void pair(code);
        }}
      >
        <label>
          Single-use pairing code
          <Input
            autoComplete="off"
            spellcheck={false}
            value={code}
            aria-invalid={rejected || undefined}
            aria-describedby={rejected ? "pairing-rejected" : undefined}
            onInput={(event) => {
              setCode(event.currentTarget.value);
              setRejected(false);
            }}
            required
          />
        </label>
        {rejected && (
          <Notice tone="error" id="pairing-rejected">
            This code wasn’t accepted. Run <code>uagent --web</code> for a new
            code.
          </Notice>
        )}
        <Button type="submit" variant="primary" busy={busy} disabled={!online}>
          Connect device
        </Button>
      </form>
      {!online && (
        <Notice tone="warning">Reconnect to pair this device.</Notice>
      )}
      <p class="muted">
        The host must remain awake and reachable. An installed app may need its
        own pairing code.
      </p>
    </main>
  );
}
