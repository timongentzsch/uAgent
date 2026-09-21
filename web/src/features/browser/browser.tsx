import { useEffect, useRef, useState } from "preact/hooks";
import type { Report, Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import RFB from "@novnc/novnc";
import Trackpad from "./trackpad.tsx";
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
  const textBox = useRef<HTMLTextAreaElement>(null);
  const cursor = useRef({ x: 0.5, y: 0.5 });
  const pasteTimer = useRef<number | null>(null);
  const [connection, setConnection] = useState("Connecting…");
  const [fit, setFit] = useState(true);
  const [panel, setPanel] = useState<"trackpad" | "text" | null>(() =>
    matchMedia("(pointer: coarse)").matches ? "trackpad" : null,
  );
  const [text, setText] = useState("");
  const [remoteText, setRemoteText] = useState("");
  const [notice, setNotice] = useState("");
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
    rfb.addEventListener("clipboard", (event) => {
      setRemoteText((event as CustomEvent<{ text: string }>).detail.text);
    });
    rfb.addEventListener("securityfailure", () =>
      report(new Error("Browser viewer authentication failed")),
    );
    return () => {
      if (pasteTimer.current !== null) clearTimeout(pasteTimer.current);
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

  const canvas = () => target.current?.querySelector("canvas");
  const point = () => {
    const element = canvas();
    if (!element) return null;
    const rect = element.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    return {
      element,
      rect,
      x: rect.left + cursor.current.x * rect.width,
      y: rect.top + cursor.current.y * rect.height,
    };
  };
  const mouse = (
    kind: "mousedown" | "mouseup" | "mousemove",
    button = 0,
    buttons = 0,
  ) => {
    const position = point();
    if (!position || connection !== "Connected") return;
    position.element.dispatchEvent(
      new MouseEvent(kind, {
        bubbles: true,
        cancelable: true,
        clientX: position.x,
        clientY: position.y,
        button,
        buttons,
      }),
    );
  };
  const move = (dx: number, dy: number) => {
    const position = point();
    if (!position) return;
    cursor.current.x = Math.max(
      0,
      Math.min(1, cursor.current.x + (dx * 1.4) / position.rect.width),
    );
    cursor.current.y = Math.max(
      0,
      Math.min(1, cursor.current.y + (dy * 1.4) / position.rect.height),
    );
    mouse("mousemove", 0, holding.current ? 1 : 0);
  };
  const holding = useRef(false);
  const button = (which: 0 | 2, down: boolean) => {
    if (which === 0) holding.current = down;
    mouse(
      down ? "mousedown" : "mouseup",
      which,
      down ? (which === 0 ? 1 : 2) : 0,
    );
  };
  const scroll = (dy: number) => {
    const position = point();
    if (!position || connection !== "Connected") return;
    position.element.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: position.x,
        clientY: position.y,
        deltaY: dy,
      }),
    );
  };
  const key = (keysym: number, code?: string) =>
    viewer.current?.sendKey(keysym, code);
  const chord = (keysym: number, code: string) => {
    const rfb = viewer.current;
    if (!rfb || connection !== "Connected") return;
    rfb.sendKey(0xffe3, "ControlLeft", true);
    rfb.sendKey(keysym, code);
    rfb.sendKey(0xffe3, "ControlLeft", false);
  };
  const sendText = (paste: boolean) => {
    const rfb = viewer.current;
    if (!rfb || connection !== "Connected" || !text) return;
    rfb.clipboardPasteFrom(text);
    setNotice(
      paste
        ? "Text sent and pasted into Chrome."
        : "Text sent to Chrome’s clipboard.",
    );
    if (paste) {
      if (pasteTimer.current !== null) clearTimeout(pasteTimer.current);
      pasteTimer.current = window.setTimeout(() => {
        if (viewer.current === rfb) chord(0x76, "KeyV");
        pasteTimer.current = null;
      }, 150);
    }
  };
  const copyToDevice = async () => {
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
      setNotice("Copied to this device.");
    } catch {
      textBox.current?.focus();
      textBox.current?.select();
      setNotice(
        document.execCommand("copy")
          ? "Copied to this device."
          : "Text selected. Use your device’s Copy command.",
      );
    }
  };
  const toggleFit = () => {
    setFit(!fit);
    setPanel(
      fit ? null : matchMedia("(pointer: coarse)").matches ? "trackpad" : null,
    );
  };
  return (
    <>
      <div
        class="browser-screen"
        ref={target}
        aria-label="Interactive browser display"
        onPointerDown={(event) => {
          if (!(event.target instanceof HTMLCanvasElement)) return;
          const rect = event.target.getBoundingClientRect();
          cursor.current = {
            x: (event.clientX - rect.left) / rect.width,
            y: (event.clientY - rect.top) / rect.height,
          };
        }}
      />
      <div class="browser-view-controls">
        <small role="status" class="muted">
          {connection}
        </small>
        <button type="button" onClick={toggleFit}>
          {fit ? "Actual size" : "Fit screen"}
        </button>
        {fit && (
          <button
            type="button"
            class="browser-touch-toggle"
            aria-pressed={panel === "trackpad"}
            onClick={() => setPanel(panel === "trackpad" ? null : "trackpad")}
          >
            Trackpad
          </button>
        )}
        <button
          type="button"
          aria-pressed={panel === "text"}
          onClick={() => setPanel(panel === "text" ? null : "text")}
        >
          Text &amp; keys
        </button>
      </div>
      {panel === "trackpad" && fit && (
        <Trackpad
          onMove={move}
          onButton={button}
          onScroll={scroll}
          disabled={connection !== "Connected"}
        />
      )}
      {panel === "text" && (
        <div class="browser-text-panel">
          <label for="browser-text">Text for Chrome</label>
          <textarea
            id="browser-text"
            ref={textBox}
            value={text}
            onInput={(event) => setText(event.currentTarget.value)}
            placeholder="Type or paste text here"
            autocapitalize="off"
            spellcheck={false}
          />
          <div class="browser-text-actions">
            <button
              type="button"
              disabled={connection !== "Connected" || !text}
              onClick={() => sendText(true)}
            >
              Send &amp; paste
            </button>
            <button
              type="button"
              disabled={connection !== "Connected" || !text}
              onClick={() => sendText(false)}
            >
              Send only
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => {
                setText(remoteText);
                setNotice(
                  remoteText
                    ? "Chrome clipboard loaded."
                    : "Chrome clipboard is empty.",
                );
              }}
            >
              Get from Chrome
            </button>
            <button
              type="button"
              disabled={!text}
              onClick={() => void copyToDevice()}
            >
              Copy to device
            </button>
          </div>
          <div class="browser-key-actions" aria-label="Browser keys">
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => chord(0x6c, "KeyL")}
            >
              Address
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => key(0xff09, "Tab")}
            >
              Tab
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => key(0xff0d, "Enter")}
            >
              Enter
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => key(0xff08, "Backspace")}
            >
              ⌫
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => key(0xff1b, "Escape")}
            >
              Esc
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => chord(0x63, "KeyC")}
            >
              Copy
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => chord(0x76, "KeyV")}
            >
              Paste
            </button>
            <button
              type="button"
              disabled={connection !== "Connected"}
              onClick={() => chord(0x61, "KeyA")}
            >
              Select all
            </button>
          </div>
          <small role="status" class="muted">
            {notice ||
              "Place the cursor in Chrome before sending text or keys."}
          </small>
        </div>
      )}
      {panel === "trackpad" && (
        <small class="muted browser-trackpad-hint">
          Move to aim · tap to click · two fingers to scroll or right-click ·
          hold Left and move to drag.
        </small>
      )}
      {!fit && (
        <small class="muted">
          Drag the browser to pan. Fit screen restores the trackpad.
        </small>
      )}
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
      {!status.controller && (
        <p class="muted">
          This is the same Chrome tab the agent uses. Sign in here, including
          MFA; credentials stay in the browser. One paired device controls it at
          a time.
        </p>
      )}
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
