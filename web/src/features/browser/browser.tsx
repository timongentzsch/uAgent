import {
  StatusLed,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import type { ComponentChildren, RefObject } from "preact";
import { failure, type Report, type Session } from "../../shared/types.ts";
import { api, command } from "../../state/api.ts";
import RFB from "@novnc/novnc";
import {
  Button,
  IconButton,
  Input,
  Select,
  Textarea,
} from "../../shared/ui.tsx";
import { SheetButton } from "../../shared/sheet.tsx";
import { ChevronDown, Keyboard } from "lucide-preact";
import BrowserTouch from "./touch.tsx";
import { hideTouchCursor, sendPointer } from "./rfb.ts";
import { BrowserFrame, BrowserTools, touchBrowser } from "./frame.tsx";

const VIEWER_RECONNECT_DELAY_MS = 1_000;
const CLIPBOARD_PASTE_DELAY_MS = 150;
const CLIPBOARD_COPY_TIMEOUT_MS = 1_500;
const STATUS_POLL_INTERVAL_MS = 2_000;
const TOAST_MS = 3_000;
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
  v: 0x76,
});

interface BrowserStatus {
  ok?: boolean;
  running?: boolean;
  mode?: "idle" | "agent" | "human";
  waiting?: boolean;
  controller?: boolean;
  leased?: boolean;
  session_id?: string;
  interaction_id?: string;
  display?: number;
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
const SPECIAL_KEYS: Record<string, [string, number]> = {
  Escape: ["Esc", X11_KEYSYM.escape],
  Tab: ["Tab", X11_KEYSYM.tab],
  Enter: ["Enter", X11_KEYSYM.enter],
  Backspace: ["⌫", X11_KEYSYM.backspace],
  ArrowLeft: ["←", X11_KEYSYM.left],
  ArrowUp: ["↑", X11_KEYSYM.up],
  ArrowDown: ["↓", X11_KEYSYM.down],
  ArrowRight: ["→", X11_KEYSYM.right],
};

// The remote display for one Chrome launch. Control changes only toggle
// input on the same connection; a new display remounts this.
function Screen({
  viewer,
  screen,
  readOnly,
  report,
  connection,
  setConnection,
  children,
}: {
  viewer: RefObject<RFB | null>;
  screen: RefObject<HTMLDivElement>;
  readOnly: boolean;
  report: Report;
  connection: ConnectionPhase;
  setConnection: (phase: ConnectionPhase) => void;
  children: ComponentChildren;
}) {
  const target = useRef<HTMLDivElement>(null);
  const viewOnly = useRef(readOnly);
  viewOnly.current = readOnly;
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
      const rfb = new RFB(
        target.current,
        `${scheme}//${location.host}/api/browser/viewer`,
      );
      let denied = false;
      rfb.viewOnly = viewOnly.current;
      rfb.scaleViewport = true;
      rfb.resizeSession = false;
      rfb.clipViewport = false;
      rfb.dragViewport = false;
      rfb.background = "transparent";
      hideTouchCursor(rfb);
      viewer.current = rfb;
      rfb.addEventListener("connect", () => setConnection("connected"));
      rfb.addEventListener("disconnect", () => {
        if (viewer.current === rfb) viewer.current = null;
        if (!active) return;
        setConnection(denied ? "disconnected" : "reconnecting");
        if (!denied)
          retry = window.setTimeout(connect, VIEWER_RECONNECT_DELAY_MS);
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
      viewer.current?.disconnect();
      viewer.current = null;
    };
  }, [report]);
  useEffect(() => {
    if (viewer.current) viewer.current.viewOnly = readOnly;
  }, [readOnly]);
  return (
    <BrowserTouch
      screen={screen}
      target={target}
      label="Browser viewport"
      pointer={
        readOnly
          ? undefined
          : (x, y, mask) => {
              if (viewer.current) sendPointer(viewer.current, x, y, mask);
            }
      }
    >
      {connection !== "connected" && (
        <p class="browser-overlay" role="status">
          {connection === "disconnected"
            ? "Disconnected"
            : connection === "reconnecting"
              ? "Reconnecting…"
              : "Connecting…"}
        </p>
      )}
      {children}
    </BrowserTouch>
  );
}

export default function BrowserPanel({
  sessions,
  report,
  close,
  handoff = false,
}: {
  sessions: Session[];
  report: Report;
  close: () => void;
  // Opened from the agent's request: take control at once, close on Done.
  handoff?: boolean;
}) {
  const [status, setStatus] = useState<BrowserStatus>({});
  const [busy, setBusy] = useState(false);
  const [connection, setConnection] = useState<ConnectionPhase>("connecting");
  const [typing, setTyping] = useState(false);
  const [ctrl, setCtrl] = useState(false);
  const [card, setCard] = useState<{ copy: boolean; text: string } | null>(
    null,
  );
  const [toast, setToast] = useState("");
  const [addingProfile, setAddingProfile] = useState(false);
  const [newProfile, setNewProfile] = useState("");
  const viewer = useRef<RFB | null>(null);
  const screen = useRef<HTMLDivElement>(null);
  const keyboard = useRef<HTMLTextAreaElement>(null);
  const cardText = useRef<HTMLTextAreaElement>(null);
  const pasteTimer = useRef<number | null>(null);
  const clipboardWaiters = useRef<((text: string) => void)[]>([]);
  const lifetime = useRef(new AbortController());
  const inFlight = useRef<Promise<void> | null>(null);
  const tookOver = useRef(false);

  const driving = !!status.controller;
  const otherDevice = status.mode === "human" && !!status.leased && !driving;
  const viewing = !!status.running && !otherDevice;
  const live = driving && viewing && connection === "connected";

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
      if (pasteTimer.current !== null) clearTimeout(pasteTimer.current);
    };
  }, [refresh]);
  useEffect(() => {
    if (!toast) return;
    const timer = setTimeout(() => setToast(""), TOAST_MS);
    return () => clearTimeout(timer);
  }, [toast]);

  const send = (
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
  const run = async (work: () => Promise<unknown>) => {
    setBusy(true);
    try {
      await work();
      return true;
    } catch (error) {
      report(error);
      return false;
    } finally {
      await refresh(true);
      setBusy(false);
    }
  };
  const control = (action: "takeover" | "stop" | "setup_profile") =>
    run(() => send(action));
  // Done answers the agent's request; the sheet then returns to the chat.
  const done = async () => {
    const answered = status.waiting;
    keyboard.current?.blur();
    if ((await run(() => send("done"))) && answered) close();
  };
  // Losing control closes what only a driver can use.
  useEffect(() => {
    if (live) return;
    setCard(null);
    keyboard.current?.blur();
  }, [live]);
  // Opening the browser for the agent's request takes control in the same
  // tap, once.
  useEffect(() => {
    if (!handoff || tookOver.current || !status.waiting || status.leased)
      return;
    tookOver.current = true;
    void control("takeover");
  }, [handoff, status.waiting, status.leased]);

  const key = (keysym: number) => {
    const rfb = viewer.current;
    if (!rfb || !live) return;
    if (ctrl) {
      chord(keysym);
      setCtrl(false);
    } else rfb.sendKey(keysym, "");
  };
  const chord = (keysym: number) => {
    const rfb = viewer.current;
    if (!rfb || !live) return;
    rfb.sendKey(X11_KEYSYM.control, "ControlLeft", true);
    rfb.sendKey(keysym, "");
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
      if (viewer.current === rfb) chord(X11_KEYSYM.v);
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
  useEffect(() => {
    const rfb = viewer.current;
    if (!rfb) return;
    const copied = (event: Event) => {
      const text = (event as CustomEvent<{ text: string }>).detail.text;
      for (const resolve of clipboardWaiters.current.splice(0)) resolve(text);
    };
    rfb.addEventListener("clipboard", copied);
    return () => rfb.removeEventListener("clipboard", copied);
  }, [connection]);
  // Copy in Chrome, then hand the text to this device. The promise keeps the
  // tap's user activation alive while the remote clipboard arrives; without
  // clipboard access the card shows the text, selected, for the system menu.
  const copy = async (letter: "c" | "x" | null = "c") => {
    if (!live) return;
    const copied = nextRemoteCopy();
    if (letter) chord(letter.charCodeAt(0));
    try {
      await navigator.clipboard.write([
        new ClipboardItem({
          "text/plain": copied.then(
            (value) => new Blob([value], { type: "text/plain" }),
          ),
        }),
      ]);
      setToast("Copied");
    } catch {
      try {
        setCard({ copy: true, text: await copied });
        requestAnimationFrame(() => {
          cardText.current?.focus({ preventScroll: true });
          cardText.current?.select();
        });
      } catch (error) {
        setToast(failure(error).message);
      }
    }
  };
  const paste = async () => {
    if (!live) return;
    try {
      pasteIntoChrome(await navigator.clipboard.readText());
    } catch {
      setCard({ copy: false, text: "" });
      requestAnimationFrame(() =>
        cardText.current?.focus({ preventScroll: true }),
      );
    }
  };
  const actions = useRef({ copy, paste, chord });
  actions.current = { copy, paste, chord };

  // Desktop shortcuts inside the viewer: paste carries this device's
  // clipboard to Chrome first, copy brings Chrome's clipboard back, and on
  // Apple keyboards Command stands in for Ctrl.
  useEffect(() => {
    const element = screen.current;
    if (!element || !driving) return;
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
      else actions.current.chord(letter.charCodeAt(0));
    };
    element.addEventListener("keydown", handle, true);
    element.addEventListener("keyup", handle, true);
    return () => {
      element.removeEventListener("keydown", handle, true);
      element.removeEventListener("keyup", handle, true);
    };
  }, [driving, viewing]);

  // Live typing from the on-screen keyboard. Hardware keys arrive as keydown;
  // soft keyboards report text through beforeinput or a finished composition.
  const keyboardInput = (event: InputEvent) => {
    if (event.isComposing || event.inputType === "insertCompositionText")
      return;
    event.preventDefault();
    if (event.inputType === "insertText" && event.data) typeText(event.data);
    else if (event.inputType === "insertFromPaste")
      pasteIntoChrome(event.dataTransfer?.getData("text/plain") || "");
    else if (
      event.inputType.startsWith("insertLineBreak") ||
      event.inputType === "insertParagraph"
    )
      key(X11_KEYSYM.enter);
    else if (event.inputType.startsWith("deleteContentBackward"))
      key(X11_KEYSYM.backspace);
  };
  const keepFocus = (event: Event) => event.preventDefault();

  const profileName =
    status.profiles?.find((profile) => profile.id === status.profile_id)
      ?.name || "Default";
  const canChangeProfile =
    driving || (status.mode === "idle" && !status.running && !status.leased);
  // Short enough to fit beside the tools on a phone; the menu says more.
  const state = !status.ok
    ? "Browser"
    : otherDevice
      ? "In use"
      : driving
        ? status.waiting
          ? "Your turn"
          : "Driving"
        : status.waiting
          ? "Needs you"
          : status.mode === "agent" && status.running
            ? "Agent"
            : status.running
              ? "Watching"
              : "Stopped";
  const detail = !status.ok
    ? "Loading"
    : otherDevice
      ? "Another device is driving"
      : driving
        ? status.waiting
          ? "The agent is waiting for you"
          : "You're driving"
        : status.waiting
          ? "The agent needs you"
          : status.mode === "agent" && status.running
            ? "The agent is working"
            : status.running
              ? "Watching"
              : "Chrome is stopped";
  const createProfile = (event: Event) => {
    event.preventDefault();
    void run(async () => {
      const created = await send("create_profile", { name: newProfile });
      if ("result" in created && created.result.created_profile_id) {
        await send("select_profile", {
          profile_id: created.result.created_profile_id,
        });
        setNewProfile("");
        setAddingProfile(false);
      }
    });
  };
  // Status, page, profiles and stopping share one menu on the status pill.
  const statusMenu = (
    <SheetButton
      label={`${detail}. Browser status and profiles`}
      className="browser-status-menu"
      buttonClass="quiet browser-status"
      heading="Browser"
      disabled={!status.ok}
      trigger={
        <>
          <StatusLed
            state={
              driving
                ? "active"
                : status.mode === "agent" && status.running
                  ? "running"
                  : "idle"
            }
          />
          <span>
            {state}
            {profileName !== "Default" && ` · ${profileName}`}
          </span>
          <ChevronDown />
        </>
      }
    >
      <div class="browser-profile">
        <strong>{detail}</strong>
        {status.url && (
          <small class="muted browser-page" title={status.url}>
            {status.title || status.url}
          </small>
        )}
        <Select
          aria-label="Chrome profile"
          value={status.profile_id || "default"}
          disabled={busy || !canChangeProfile}
          onChange={(event) =>
            void run(() =>
              send("select_profile", {
                profile_id: event.currentTarget.value,
              }),
            )
          }
        >
          {status.profiles?.map((profile) => (
            <option key={profile.id} value={profile.id}>
              {profile.name}
            </option>
          ))}
        </Select>
        {!canChangeProfile && (
          <small class="muted">Take over to switch profiles.</small>
        )}
        <div class="browser-profile-controls">
          <Button
            disabled={busy || !canChangeProfile}
            onClick={() => setAddingProfile(!addingProfile)}
          >
            New profile
          </Button>
          {driving && !status.profile_setup && (
            <Button
              disabled={busy}
              title="Reopens this profile for manual sign-in. The agent waits until you're done."
              onClick={() => void control("setup_profile")}
            >
              Sign in to profile
            </Button>
          )}
          {status.running && status.mode === "idle" && (
            <Button disabled={busy} onClick={() => void control("stop")}>
              Stop browser
            </Button>
          )}
        </div>
        {addingProfile && (
          <form class="browser-profile-create" onSubmit={createProfile}>
            <Input
              aria-label="New Chrome profile name"
              value={newProfile}
              onInput={(event) => setNewProfile(event.currentTarget.value)}
              placeholder="e.g. Work or Personal"
              required
            />
            <Button type="submit" disabled={busy || !newProfile.trim()}>
              Create and use
            </Button>
          </form>
        )}
      </div>
    </SheetButton>
  );

  // One message at most sits on the screen's top edge.
  const banner =
    status.error ||
    (driving && status.profile_setup
      ? "Sign in to your sites. Done reopens this profile for the agent with your saved logins."
      : !driving && status.waiting && viewing
        ? "The agent is waiting for you. Take over to continue."
        : "");
  const overlay = !status.ok
    ? "Loading browser…"
    : otherDevice
      ? "Another device is driving. Closing the browser there hands it back to the agent."
      : !status.running
        ? status.waiting
          ? "The agent needs you in Chrome. Take over to continue."
          : "Chrome is stopped. Take over to start it and sign in to sites; the agent uses your logins."
        : "";

  const overlays = (
    <>
      {banner && (
        <p class="browser-banner" role="status">
          {banner}
        </p>
      )}
      {toast && (
        <p class="browser-toast" role="status">
          {toast}
        </p>
      )}
      {card && (
        <form
          class="browser-card"
          onSubmit={(event) => {
            event.preventDefault();
            if (!card.copy) pasteIntoChrome(card.text);
            setCard(null);
          }}
        >
          <label for="browser-card-text">
            {card.copy
              ? "Copied from Chrome. Use your device’s Copy."
              : "Paste here to send it to Chrome."}
          </label>
          <Textarea
            id="browser-card-text"
            inputRef={cardText}
            value={card.text}
            onInput={(event) =>
              setCard({ ...card, text: event.currentTarget.value })
            }
            autocapitalize="off"
            spellcheck={false}
          />
          <div class="browser-card-actions">
            <Button onClick={() => setCard(null)}>
              {card.copy ? "Done" : "Cancel"}
            </Button>
            {!card.copy && (
              <Button
                type="submit"
                variant="primary"
                disabled={!card.text || !live}
              >
                Send
              </Button>
            )}
          </div>
        </form>
      )}
      {touchBrowser && driving && (
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
            setCtrl(false);
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
            key(special[1]);
          }}
        />
      )}
    </>
  );

  return (
    <BrowserFrame
      screen={
        viewing ? (
          <Screen
            key={status.display}
            viewer={viewer}
            screen={screen}
            readOnly={!driving}
            report={report}
            connection={connection}
            setConnection={setConnection}
          >
            {overlays}
          </Screen>
        ) : (
          <div class="browser-screen">
            <p class="browser-overlay" role="status">
              {overlay}
            </p>
            {overlays}
          </div>
        )
      }
      bar={
        typing ? (
          <>
            <div class="browser-keys" aria-label="Special keys">
              {Object.entries(SPECIAL_KEYS)
                .filter(([name]) => name !== "Enter" && name !== "Backspace")
                .map(([name, [label, sym]]) => (
                  <Button
                    key={name}
                    aria-label={name.replace("Arrow", "Arrow ")}
                    onPointerDown={keepFocus}
                    onClick={() => key(sym)}
                  >
                    {label}
                  </Button>
                ))}
              <Button
                aria-pressed={ctrl}
                onPointerDown={keepFocus}
                onClick={() => setCtrl(!ctrl)}
              >
                Ctrl
              </Button>
            </div>
            <IconButton
              label="Hide keyboard"
              onPointerDown={keepFocus}
              onClick={() => keyboard.current?.blur()}
            >
              <Keyboard />
            </IconButton>
          </>
        ) : (
          <>
            {statusMenu}
            <BrowserTools
              disabled={!live}
              keyboard={() => keyboard.current?.focus()}
              copy={() => void copy()}
              paste={() => void paste()}
            />
            {driving ? (
              <Button
                variant="primary"
                class="browser-primary"
                disabled={busy}
                title="Hand the browser back to the agent"
                onClick={() => void done()}
              >
                Done
              </Button>
            ) : (
              <Button
                variant="primary"
                class="browser-primary"
                disabled={busy || otherDevice || !status.ok}
                onClick={() => void control("takeover")}
              >
                Take over
              </Button>
            )}
          </>
        )
      }
    />
  );
}
