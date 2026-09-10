import "./pairing.css";
import { useState } from "preact/hooks";
import { api } from "./store.ts";
import { Mark } from "./ui.tsx";
export default function Pairing({
  paired,
  report,
}: {
  paired: () => Promise<void>;
  report: (error: unknown) => void;
}) {
  const [code, setCode] = useState("");
  const [busy, setBusy] = useState(false);
  return (
    <main class="pairing">
      <Mark className="wordmark" />
      <h1>Your local coding workspace.</h1>
      <p>
        Connect this device to the µAgent host. Run <code>uagent --web</code> on
        that computer for a fresh pairing code.
      </p>
      <form
        onSubmit={async (event) => {
          event.preventDefault();
          setBusy(true);
          try {
            await api("/api/auth", {
              code: code.trim(),
              name: /iPhone|iPad/.test(navigator.userAgent)
                ? "iOS device"
                : /Android/.test(navigator.userAgent)
                  ? "Android device"
                  : "Browser",
            });
            await paired();
          } catch (error) {
            report(error);
          } finally {
            setBusy(false);
          }
        }}
      >
        <label>
          Single-use pairing code
          <input
            autoComplete="off"
            spellcheck={false}
            value={code}
            onInput={(event) => setCode(event.currentTarget.value)}
            required
          />
        </label>
        <button class="primary" disabled={busy || !navigator.onLine}>
          {busy ? "Connecting…" : "Connect device"}
        </button>
      </form>
      <p class="muted">
        The host must remain awake and reachable. An installed app may need its
        own pairing code.
      </p>
    </main>
  );
}
