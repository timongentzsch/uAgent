import { useEffect, useState } from "preact/hooks";
import { api } from "../../state/api.ts";
import { failure } from "../../shared/types.ts";
import { Button, Mark, Input } from "../../shared/ui.tsx";
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
  const [busy, setBusy] = useState(false);
  // The host refused the code: used, mistyped or too old.
  const [expired, setExpired] = useState(false);
  async function pair(value: string) {
    setBusy(true);
    setExpired(false);
    try {
      await api("/api/auth", {
        code: value.trim(),
        name: /iPhone|iPad/.test(navigator.userAgent)
          ? "iOS device"
          : /Android/.test(navigator.userAgent)
            ? "Android device"
            : "Browser",
      });
      await paired();
    } catch (error) {
      if (failure(error).status === 403) setExpired(true);
      else report(error);
    } finally {
      setBusy(false);
    }
  }
  // A pairing link connects without typing.
  useEffect(() => {
    const linked = linkedCode();
    if (!linked) return;
    setCode(linked);
    void pair(linked);
  }, []);
  return (
    <main class="pairing">
      <Mark className="wordmark" />
      <h1>Your local coding workspace.</h1>
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
            aria-invalid={expired || undefined}
            aria-describedby={expired ? "pairing-expired" : undefined}
            onInput={(event) => {
              setCode(event.currentTarget.value);
              setExpired(false);
            }}
            required
          />
        </label>
        {expired && (
          <p id="pairing-expired" role="alert" class="pairing-expired">
            Expired — run <code>uagent --web</code> again for a new code.
          </p>
        )}
        <Button
          type="submit"
          variant="primary"
          busy={busy}
          disabled={!navigator.onLine}
        >
          Connect device
        </Button>
      </form>
      <p class="muted">
        The host must remain awake and reachable. An installed app may need its
        own pairing code.
      </p>
    </main>
  );
}
