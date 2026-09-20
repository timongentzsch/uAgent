import { useEffect, useRef, useState } from "preact/hooks";
import type { Report, Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import RFB from "@novnc/novnc";
import "./browser.css";

interface BrowserStatus {
  ok?: boolean;
  running?: boolean;
  mode?: "idle" | "agent" | "human";
  controller?: boolean;
  leased?: boolean;
  session_id?: string;
  interaction_id?: string;
  generation?: number;
  url?: string;
  title?: string;
  error?: string;
}

function Viewer({ report }: { report: Report }) {
  const target = useRef<HTMLDivElement>(null);
  const viewer = useRef<RFB | null>(null);
  const [connection, setConnection] = useState("Connecting…");
  const [fit, setFit] = useState(true);
  useEffect(() => {
    if (!target.current) return;
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const rfb = new RFB(
      target.current,
      `${scheme}//${location.host}/api/browser/viewer`,
    );
    rfb.scaleViewport = true;
    rfb.resizeSession = false;
    viewer.current = rfb;
    rfb.addEventListener("connect", () => setConnection("Connected"));
    rfb.addEventListener("disconnect", () => setConnection("Disconnected"));
    rfb.addEventListener("securityfailure", () =>
      report(new Error("Browser viewer authentication failed")),
    );
    return () => {
      viewer.current = null;
      rfb.disconnect();
    };
  }, [report]);
  useEffect(() => {
    const rfb = viewer.current;
    if (!rfb) return;
    rfb.scaleViewport = fit;
    rfb.clipViewport = !fit;
    rfb.dragViewport = !fit;
  }, [fit]);
  return (
    <>
      <div
        class="browser-screen"
        ref={target}
        aria-label="Interactive browser display"
      />
      <div class="browser-view-controls">
        <small role="status" class="muted">
          {connection}
        </small>
        <button type="button" onClick={() => setFit(!fit)}>
          {fit ? "Actual size" : "Fit screen"}
        </button>
      </div>
      {!fit && <small class="muted">Drag the browser to pan.</small>}
    </>
  );
}

export default function BrowserPanel({
  sessions,
  report,
}: {
  sessions: Session[];
  report: Report;
}) {
  const [status, setStatus] = useState<BrowserStatus>({});
  const [busy, setBusy] = useState(false);
  const refresh = async () => {
    try {
      setStatus(await api<BrowserStatus>("/api/browser/status"));
    } catch (error) {
      report(error);
    }
  };
  useEffect(() => {
    void refresh();
    const timer = setInterval(() => void refresh(), 2000);
    return () => clearInterval(timer);
  }, []);
  const control = async (action: "takeover" | "done" | "stop") => {
    setBusy(true);
    try {
      const session = sessions.find((item) => item.id === status.session_id);
      await command("browser", session, {
        action,
        interaction_id: status.interaction_id || "",
      });
      await refresh();
    } catch (error) {
      report(error);
    } finally {
      setBusy(false);
    }
  };
  return (
    <section class="browser-panel">
      <p class="muted">
        This is the same Chrome tab the agent uses. Sign in here, including MFA;
        credentials stay in the browser. One paired device controls it at a
        time.
      </p>
      {status.error && <p role="alert">{status.error}</p>}
      {status.url && (
        <p class="browser-location" title={status.title}>
          {status.url}
        </p>
      )}
      {status.mode === "human" && status.leased && !status.controller ? (
        <p>
          Another device controls the browser. Its connection can close without
          resuming the agent.
        </p>
      ) : status.controller ? (
        <>
          <Viewer key={status.generation} report={report} />
          <div class="dialog-actions">
            <button
              class="primary"
              disabled={busy}
              onClick={() => void control("done")}
            >
              Done
            </button>
          </div>
        </>
      ) : (
        <div class="dialog-actions">
          {status.mode === "human" ||
          status.mode === "idle" ||
          status.mode === "agent" ? (
            <button
              class="primary"
              disabled={busy}
              onClick={() => void control("takeover")}
            >
              Take control
            </button>
          ) : null}
          {status.running && status.mode === "idle" && (
            <button disabled={busy} onClick={() => void control("stop")}>
              Stop browser
            </button>
          )}
        </div>
      )}
    </section>
  );
}
