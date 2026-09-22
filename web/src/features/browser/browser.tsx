import { useEffect, useRef, useState } from "preact/hooks";
import type { Report, Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import RFB from "@novnc/novnc";
import BrowserTouch from "./touch.tsx";
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
  profile_id?: string;
  profiles?: { id: string; name: string }[];
  error?: string;
}

function Viewer({ report, readOnly }: { report: Report; readOnly: boolean }) {
  const screen = useRef<HTMLDivElement>(null);
  const target = useRef<HTMLDivElement>(null);
  const viewer = useRef<RFB | null>(null);
  const textBox = useRef<HTMLTextAreaElement>(null);
  const pasteTimer = useRef<number | null>(null);
  const [connection, setConnection] = useState("Connecting…");
  const [showText, setShowText] = useState(false);
  const [text, setText] = useState("");
  const [remoteText, setRemoteText] = useState("");
  const [notice, setNotice] = useState("");

  useEffect(() => {
    if (!target.current) return;
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    let active = true;
    let retry: number | undefined;
    const connect = () => {
      if (!active || !target.current) return;
      setConnection("Connecting…");
      const role = readOnly ? "observe" : "control";
      const rfb = new RFB(
        target.current,
        `${scheme}//${location.host}/api/browser/viewer?role=${role}`,
      );
      let denied = false;
      rfb.viewOnly = readOnly;
      rfb.scaleViewport = true;
      rfb.resizeSession = false;
      rfb.clipViewport = false;
      rfb.dragViewport = false;
      viewer.current = rfb;
      rfb.addEventListener("connect", () => setConnection("Connected"));
      rfb.addEventListener("disconnect", () => {
        if (viewer.current === rfb) viewer.current = null;
        if (active) {
          setConnection("Disconnected");
          if (!denied) retry = window.setTimeout(connect, 1000);
        }
      });
      rfb.addEventListener("clipboard", (event) => {
        setRemoteText((event as CustomEvent<{ text: string }>).detail.text);
      });
      rfb.addEventListener("securityfailure", () => {
        denied = true;
        report(new Error("Browser viewer authentication failed"));
      });
    };
    connect();
    return () => {
      active = false;
      if (retry !== undefined) clearTimeout(retry);
      if (pasteTimer.current !== null) clearTimeout(pasteTimer.current);
      viewer.current?.disconnect();
      viewer.current = null;
    };
  }, [readOnly, report]);

  const key = (keysym: number, code?: string) =>
    viewer.current?.sendKey(keysym, code);
  const chord = (keysym: number, code: string) => {
    const rfb = viewer.current;
    if (!rfb || connection !== "Connected" || readOnly) return;
    rfb.sendKey(0xffe3, "ControlLeft", true);
    rfb.sendKey(keysym, code);
    rfb.sendKey(0xffe3, "ControlLeft", false);
  };
  const sendText = (paste: boolean) => {
    const rfb = viewer.current;
    if (!rfb || connection !== "Connected" || !text || readOnly) return;
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

  return (
    <>
      <div
        class={`browser-screen${readOnly ? " readonly" : ""}`}
        ref={screen}
        aria-label={
          readOnly ? "Read-only browser display" : "Interactive browser display"
        }
      >
        <div class="browser-rfb" ref={target} />
        <BrowserTouch
          screen={screen}
          target={target}
          rfb={viewer}
          disabled={readOnly || connection !== "Connected"}
        />
      </div>
      <div class="browser-view-controls">
        <small role="status" class="muted">
          {readOnly
            ? `Watching agent · ${connection.toLowerCase()}`
            : connection}
        </small>
        {!readOnly && (
          <button
            type="button"
            aria-pressed={showText}
            onClick={() => setShowText(!showText)}
          >
            Text &amp; keys
          </button>
        )}
      </div>
      {showText && !readOnly && (
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
  const [addingProfile, setAddingProfile] = useState(false);
  const [profileName, setProfileName] = useState("");
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
  const send = async (
    action: "takeover" | "done" | "stop" | "create_profile" | "select_profile",
    fields: { name?: string; profile_id?: string } = {},
  ) => {
    const session = sessions.find((item) => item.id === status.session_id);
    return command("browser", session, {
      action,
      interaction_id: status.interaction_id || "",
      ...fields,
    });
  };
  const control = async (action: "takeover" | "done" | "stop") => {
    setBusy(true);
    try {
      await send(action);
      await refresh();
    } catch (error) {
      report(error);
    } finally {
      setBusy(false);
    }
  };
  const selectProfile = async (id: string) => {
    setBusy(true);
    try {
      await send("select_profile", { profile_id: id });
      await refresh();
    } catch (error) {
      report(error);
      await refresh();
    } finally {
      setBusy(false);
    }
  };
  const createProfile = async (event: Event) => {
    event.preventDefault();
    setBusy(true);
    try {
      const created = await send("create_profile", { name: profileName });
      if ("result" in created && created.result.created_profile_id) {
        await send("select_profile", {
          profile_id: created.result.created_profile_id,
        });
        setProfileName("");
        setAddingProfile(false);
      }
      await refresh();
    } catch (error) {
      report(error);
      await refresh();
    } finally {
      setBusy(false);
    }
  };
  const canChangeProfile =
    status.controller ||
    (status.mode === "idle" && !status.running && !status.leased);
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
      {status.profiles && (
        <div class="browser-profile">
          <label for="browser-profile-select">Chrome profile</label>
          <div class="browser-profile-controls">
            <select
              id="browser-profile-select"
              value={status.profile_id || "default"}
              disabled={busy || !canChangeProfile}
              onChange={(event) =>
                void selectProfile(event.currentTarget.value)
              }
            >
              {status.profiles.map((profile) => (
                <option key={profile.id} value={profile.id}>
                  {profile.name}
                </option>
              ))}
            </select>
            <button
              type="button"
              disabled={busy || !canChangeProfile}
              onClick={() => setAddingProfile(!addingProfile)}
            >
              New profile
            </button>
          </div>
          {!canChangeProfile && !status.error && (
            <small class="muted">Take control to switch profiles.</small>
          )}
          {addingProfile && (
            <form class="browser-profile-create" onSubmit={createProfile}>
              <input
                aria-label="New Chrome profile name"
                value={profileName}
                onInput={(event) => setProfileName(event.currentTarget.value)}
                placeholder="e.g. Work or Personal"
                required
              />
              <button type="submit" disabled={busy || !profileName.trim()}>
                Create and use
              </button>
              <button
                type="button"
                disabled={busy}
                onClick={() => setAddingProfile(false)}
              >
                Cancel
              </button>
            </form>
          )}
        </div>
      )}
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
      ) : status.controller || (status.mode === "agent" && status.running) ? (
        <>
          <Viewer
            key={`${status.generation}-${status.mode}`}
            report={report}
            readOnly={!status.controller}
          />
          <div class="dialog-actions">
            {status.controller ? (
              <button
                class="primary"
                disabled={busy}
                onClick={() => void control("done")}
              >
                Done
              </button>
            ) : (
              <button
                class="primary"
                disabled={busy}
                onClick={() => void control("takeover")}
              >
                Take control
              </button>
            )}
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
