import {
  ConnectionStatus,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import { failure, type Report, type Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import RFB from "@novnc/novnc";
import { Spinner, Input, Textarea, Select } from "../../shared/ui.tsx";
import { MenuItem, Popover } from "../../shared/popover.tsx";
import { ClipboardPaste, Copy, Keyboard } from "lucide-preact";
import BrowserInput from "./input.tsx";
import "./browser.css";

const VIEWER_RECONNECT_DELAY_MS = 1_000;
const CLIPBOARD_PASTE_DELAY_MS = 150;
const CLIPBOARD_COPY_TIMEOUT_MS = 1_500;
const STATUS_POLL_INTERVAL_MS = 2_000;
const X11_KEYSYM = Object.freeze({
  control: 0xffe3,
  tab: 0xff09,
  enter: 0xff0d,
  backspace: 0xff08,
  escape: 0xff1b,
  left: 0xff51,
  up: 0xff52,
  right: 0xff53,
  down: 0xff54,
  a: 0x61,
  c: 0x63,
  l: 0x6c,
  v: 0x76,
});

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
  profile_setup?: boolean;
  profiles?: { id: string; name: string }[];
  error?: string;
}

// Remote Chrome runs on Linux, so the platform shortcut modifier maps to Ctrl.
const APPLE = /Mac|iPhone|iPad/.test(navigator.platform);
// X11 keysyms: Latin-1 is identity; anything else uses the Unicode range.
const keysymFor = (codepoint: number) =>
  codepoint >= 0x20 && codepoint <= 0xff ? codepoint : 0x01000000 | codepoint;
// Soft keyboards report no deletion from an empty field, so the hidden
// keyboard input always holds this one sentinel character.
const KEYBOARD_SENTINEL = " ";
const SPECIAL_KEYS: Record<string, [string, number, string]> = {
  Escape: ["Esc", X11_KEYSYM.escape, "Escape"],
  Tab: ["Tab", X11_KEYSYM.tab, "Tab"],
  Enter: ["Enter", X11_KEYSYM.enter, "Enter"],
  Backspace: ["⌫", X11_KEYSYM.backspace, "Backspace"],
  ArrowLeft: ["←", X11_KEYSYM.left, "ArrowLeft"],
  ArrowUp: ["↑", X11_KEYSYM.up, "ArrowUp"],
  ArrowDown: ["↓", X11_KEYSYM.down, "ArrowDown"],
  ArrowRight: ["→", X11_KEYSYM.right, "ArrowRight"],
};

function Viewer({ report, readOnly }: { report: Report; readOnly: boolean }) {
  const screen = useRef<HTMLDivElement>(null);
  const target = useRef<HTMLDivElement>(null);
  const viewer = useRef<RFB | null>(null);
  const textBox = useRef<HTMLTextAreaElement>(null);
  const keyboard = useRef<HTMLTextAreaElement>(null);
  const pasteTimer = useRef<number | null>(null);
  const clipboardWaiters = useRef<((text: string) => void)[]>([]);
  const [connection, setConnection] = useState<ConnectionPhase>("connecting");
  const [touch] = useState(() => matchMedia("(pointer: coarse)").matches);
  const [typing, setTyping] = useState(false);
  const [control, setControl] = useState(false);
  const [showText, setShowText] = useState(false);
  const [text, setText] = useState("");
  const [remoteText, setRemoteText] = useState("");
  const [notice, setNotice] = useState("");
  const live = connection === "connected" && !readOnly;

  useEffect(() => {
    if (!target.current) return;
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    let active = true;
    let attempted = false;
    let retry: number | undefined;
    const connect = () => {
      if (!active || !target.current) return;
      setConnection(attempted ? "reconnecting" : "connecting");
      attempted = true;
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
      rfb.addEventListener("connect", () => setConnection("connected"));
      rfb.addEventListener("disconnect", () => {
        if (viewer.current === rfb) viewer.current = null;
        if (active) {
          setConnection(denied ? "disconnected" : "reconnecting");
          if (!denied)
            retry = window.setTimeout(connect, VIEWER_RECONNECT_DELAY_MS);
        }
      });
      rfb.addEventListener("clipboard", (event) => {
        const copied = (event as CustomEvent<{ text: string }>).detail.text;
        setRemoteText(copied);
        for (const resolve of clipboardWaiters.current.splice(0))
          resolve(copied);
      });
      rfb.addEventListener("securityfailure", () => {
        denied = true;
        setConnection("disconnected");
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

  const key = (keysym: number, code?: string) => {
    if (!live) return;
    if (control) {
      chord(keysym, code || "");
      setControl(false);
    } else viewer.current?.sendKey(keysym, code);
  };
  const chord = (keysym: number, code: string) => {
    const rfb = viewer.current;
    if (!rfb || !live) return;
    rfb.sendKey(X11_KEYSYM.control, "ControlLeft", true);
    rfb.sendKey(keysym, code);
    rfb.sendKey(X11_KEYSYM.control, "ControlLeft", false);
  };
  const typeText = (value: string) => {
    for (const character of value) key(keysymFor(character.codePointAt(0)!));
  };
  const pasteIntoChrome = (value: string) => {
    const rfb = viewer.current;
    if (!rfb || !live || !value) return;
    rfb.clipboardPasteFrom(value);
    if (pasteTimer.current !== null) clearTimeout(pasteTimer.current);
    pasteTimer.current = window.setTimeout(() => {
      if (viewer.current === rfb) chord(X11_KEYSYM.v, "KeyV");
      pasteTimer.current = null;
    }, CLIPBOARD_PASTE_DELAY_MS);
  };
  const nextRemoteCopy = () =>
    new Promise<string>((resolve, reject) => {
      const waiter = (copied: string) => {
        clearTimeout(timer);
        resolve(copied);
      };
      const timer = window.setTimeout(() => {
        clipboardWaiters.current = clipboardWaiters.current.filter(
          (item) => item !== waiter,
        );
        reject(new Error("Nothing was copied in Chrome."));
      }, CLIPBOARD_COPY_TIMEOUT_MS);
      clipboardWaiters.current.push(waiter);
    });
  // Copy in Chrome, then hand the text to this device. The promise keeps the
  // click's user activation alive while the remote clipboard arrives.
  const copy = async (letter: "c" | "x" | null = "c") => {
    if (!live) return;
    const copied = nextRemoteCopy();
    if (letter) chord(letter.charCodeAt(0), `Key${letter.toUpperCase()}`);
    try {
      await navigator.clipboard.write([
        new ClipboardItem({
          "text/plain": copied.then(
            (value) => new Blob([value], { type: "text/plain" }),
          ),
        }),
      ]);
      setNotice("Copied to this device.");
    } catch {
      try {
        setText(await copied);
        setShowText(true);
        requestAnimationFrame(() => textBox.current?.select());
        setNotice("Text selected. Use your device’s Copy command.");
      } catch (error) {
        setNotice(failure(error).message);
      }
    }
  };
  const paste = async () => {
    if (!live) return;
    try {
      pasteIntoChrome(await navigator.clipboard.readText());
    } catch {
      setShowText(true);
      setNotice("Clipboard access was denied. Paste the text here instead.");
    }
  };
  const actions = useRef({ copy, paste, chord });
  actions.current = { copy, paste, chord };

  // Desktop shortcuts inside the viewer: paste carries this device's
  // clipboard to Chrome first, copy brings Chrome's clipboard back, and on
  // Apple keyboards Command stands in for Ctrl.
  useEffect(() => {
    const element = screen.current;
    if (!element || readOnly) return;
    const handle = (event: KeyboardEvent) => {
      if (APPLE && event.key === "Meta") {
        event.stopPropagation();
        return;
      }
      if (event.type !== "keydown" || event.altKey) return;
      const primary = APPLE
        ? event.metaKey && !event.ctrlKey
        : event.ctrlKey && !event.metaKey;
      const letter = event.key.toLowerCase();
      if (!primary || !/^[a-z]$/.test(letter)) return;
      const copying = letter === "c" || letter === "x";
      // Ctrl+C/X already reach Chrome through the viewer; only collect them.
      if (copying && !APPLE) {
        void actions.current.copy(null);
        return;
      }
      event.preventDefault();
      event.stopPropagation();
      if (letter === "v") void actions.current.paste();
      else if (copying) void actions.current.copy(letter);
      else
        actions.current.chord(
          letter.charCodeAt(0),
          `Key${letter.toUpperCase()}`,
        );
    };
    element.addEventListener("keydown", handle, true);
    element.addEventListener("keyup", handle, true);
    return () => {
      element.removeEventListener("keydown", handle, true);
      element.removeEventListener("keyup", handle, true);
    };
  }, [readOnly]);

  // Live typing from the on-screen keyboard. Hardware keys arrive as keydown;
  // soft keyboards report text through beforeinput or a finished composition.
  const keyboardInput = (event: InputEvent) => {
    if (event.isComposing || event.inputType === "insertCompositionText")
      return;
    event.preventDefault();
    if (event.inputType === "insertText" && event.data) typeText(event.data);
    else if (event.inputType.startsWith("insertLineBreak"))
      key(X11_KEYSYM.enter, "Enter");
    else if (event.inputType === "insertParagraph")
      key(X11_KEYSYM.enter, "Enter");
    else if (event.inputType.startsWith("deleteContentBackward"))
      key(X11_KEYSYM.backspace, "Backspace");
  };
  const keepFocus = (event: Event) => event.preventDefault();

  return (
    <div class="browser-viewer">
      <BrowserInput
        screen={screen}
        target={target}
        disabled={!live}
        showTrackpad={touch && !readOnly}
        readOnly={readOnly}
      />
      <div class="browser-view-controls">
        <small class="muted">
          {readOnly ? (
            <>
              Watching agent · <ConnectionStatus phase={connection} />
            </>
          ) : (
            <ConnectionStatus phase={connection} />
          )}
        </small>
        {!readOnly && (
          <>
            {touch && (
              <button
                type="button"
                class="with-icon"
                aria-pressed={typing}
                disabled={!live}
                onPointerDown={keepFocus}
                onClick={() =>
                  typing ? keyboard.current?.blur() : keyboard.current?.focus()
                }
              >
                <Keyboard aria-hidden="true" /> Keyboard
              </button>
            )}
            <button
              type="button"
              class="with-icon"
              disabled={!live}
              onClick={() => void copy()}
            >
              <Copy aria-hidden="true" /> Copy
            </button>
            <button
              type="button"
              class="with-icon"
              disabled={!live}
              onClick={() => void paste()}
            >
              <ClipboardPaste aria-hidden="true" /> Paste
            </button>
            <Popover
              label="Keys"
              trigger={<span>Keys</span>}
              buttonClass="quiet"
              side="top"
              disabled={!live}
              menu
            >
              {(close) => (
                <>
                  {Object.entries(SPECIAL_KEYS).map(([name, [label, sym]]) => (
                    <MenuItem
                      key={name}
                      aria-label={name.replace("Arrow", "Arrow ")}
                      onClick={() => {
                        key(sym, name);
                        close();
                      }}
                    >
                      {label}
                    </MenuItem>
                  ))}
                  <MenuItem
                    onClick={() => {
                      chord(X11_KEYSYM.l, "KeyL");
                      close();
                    }}
                  >
                    Address bar
                  </MenuItem>
                  <MenuItem
                    onClick={() => {
                      chord(X11_KEYSYM.a, "KeyA");
                      close();
                    }}
                  >
                    Select all
                  </MenuItem>
                  <MenuItem
                    onClick={() => {
                      setShowText(true);
                      close();
                    }}
                  >
                    Send text…
                  </MenuItem>
                </>
              )}
            </Popover>
          </>
        )}
      </div>
      {touch && !readOnly && (
        <Textarea
          inputRef={keyboard}
          class="browser-keyboard-input"
          aria-label="Type into Chrome"
          value={KEYBOARD_SENTINEL}
          autocapitalize="off"
          autocomplete="off"
          autocorrect="off"
          spellcheck={false}
          onFocus={() => setTyping(true)}
          onBlur={() => {
            setTyping(false);
            setControl(false);
          }}
          onBeforeInput={keyboardInput}
          onCompositionEnd={(event) => {
            typeText(event.data);
            event.currentTarget.value = KEYBOARD_SENTINEL;
          }}
          onKeyDown={(event) => {
            const special = SPECIAL_KEYS[event.key];
            if (!special) return;
            event.preventDefault();
            key(special[1], special[2]);
          }}
        />
      )}
      {typing && (
        <div class="browser-key-actions" aria-label="Special keys">
          {Object.entries(SPECIAL_KEYS)
            .filter(([name]) => name !== "Enter" && name !== "Backspace")
            .map(([name, [label, sym]]) => (
              <button
                key={name}
                type="button"
                aria-label={name.replace("Arrow", "Arrow ")}
                onPointerDown={keepFocus}
                onClick={() => key(sym, name)}
              >
                {label}
              </button>
            ))}
          <button
            type="button"
            aria-pressed={control}
            onPointerDown={keepFocus}
            onClick={() => setControl(!control)}
          >
            Ctrl
          </button>
        </div>
      )}
      {showText && !readOnly && (
        <div class="browser-text-panel">
          <label for="browser-text">Text for Chrome</label>
          <Textarea
            id="browser-text"
            inputRef={textBox}
            value={text}
            onInput={(event) => setText(event.currentTarget.value)}
            placeholder="Type or paste text here"
            autocapitalize="off"
            spellcheck={false}
          />
          <div class="browser-text-actions">
            <button
              type="button"
              disabled={!live || !text}
              onClick={() => {
                pasteIntoChrome(text);
                setNotice("Text sent and pasted into Chrome.");
              }}
            >
              Send &amp; paste
            </button>
            <button
              type="button"
              disabled={!live}
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
            <button type="button" onClick={() => setShowText(false)}>
              Close
            </button>
          </div>
        </div>
      )}
      {notice && !readOnly && (
        <small role="status" class="muted">
          {notice}
        </small>
      )}
    </div>
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
  const lifetime = useRef(new AbortController());
  const inFlight = useRef<Promise<void> | null>(null);
  const refresh = useCallback(
    async (afterCommand = false): Promise<void> => {
      if (inFlight.current) {
        await inFlight.current;
        if (!afterCommand) return;
      }
      while (inFlight.current) await inFlight.current;
      const { signal } = lifetime.current;
      if (signal.aborted) return;
      const request = api<BrowserStatus>("/api/browser/status", undefined, {
        signal,
      })
        .then((value) => {
          if (!signal.aborted) setStatus(value);
        })
        .catch((error) => {
          if (!signal.aborted) report(error);
        })
        .finally(() => {
          if (inFlight.current === request) inFlight.current = null;
        });
      inFlight.current = request;
      return request;
    },
    [report],
  );
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout>;
    let active = true;
    let polling = false;
    lifetime.current = new AbortController();
    const poll = async () => {
      if (polling || !active) return;
      polling = true;
      clearTimeout(timer);
      if (document.visibilityState === "visible") await refresh();
      polling = false;
      if (active) timer = setTimeout(poll, STATUS_POLL_INTERVAL_MS);
    };
    const visible = () => {
      if (document.visibilityState === "visible") void poll();
    };
    void poll();
    document.addEventListener("visibilitychange", visible);
    return () => {
      active = false;
      clearTimeout(timer);
      lifetime.current.abort();
      document.removeEventListener("visibilitychange", visible);
    };
  }, [refresh]);
  const send = async (
    action:
      | "takeover"
      | "done"
      | "stop"
      | "create_profile"
      | "select_profile"
      | "setup_profile",
    fields: { name?: string; profile_id?: string } = {},
  ) => {
    const session = sessions.find((item) => item.id === status.session_id);
    return command("browser", session, {
      action,
      interaction_id: status.interaction_id || "",
      ...fields,
    });
  };
  const control = async (
    action: "takeover" | "done" | "stop" | "setup_profile",
  ) => {
    setBusy(true);
    try {
      await send(action);
      await refresh(true);
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
      await refresh(true);
    } catch (error) {
      report(error);
      await refresh(true);
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
      await refresh(true);
    } catch (error) {
      report(error);
      await refresh(true);
    } finally {
      setBusy(false);
    }
  };
  if (!status.ok && !status.mode)
    return <Spinner label="Loading browser…" surface />;
  const canChangeProfile =
    status.controller ||
    (status.mode === "idle" && !status.running && !status.leased);
  return (
    <section class="browser-panel">
      {!status.controller && (
        <p class="muted">
          This is the Chrome profile the agent uses. Take control and choose
          Sign in to profile to save your logins, including MFA. One paired
          device controls it at a time.
        </p>
      )}
      {status.error && <p role="alert">{status.error}</p>}
      {status.profiles && (
        <div class="browser-profile">
          <label for="browser-profile-select">Chrome profile</label>
          <div class="browser-profile-controls">
            <Select
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
            </Select>
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
              <Input
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
      {status.controller && (
        <div class="browser-signin">
          {status.profile_setup ? (
            <p role="status">
              Sign in to your sites in Chrome. Done reopens this profile for the
              agent with your saved logins.
            </p>
          ) : (
            <button
              type="button"
              disabled={busy}
              title="Reopens this profile for manual sign-in. The agent waits until you choose Done."
              onClick={() => void control("setup_profile")}
            >
              Sign in to profile
            </button>
          )}
        </div>
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
