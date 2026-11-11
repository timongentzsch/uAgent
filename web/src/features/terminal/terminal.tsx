import "./terminal.css";
import { Terminal, type ITheme } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { WebLinksAddon } from "@xterm/addon-web-links";
import { useEffect, useRef, useState } from "preact/hooks";
import { ChevronDown, Keyboard, Plus, X } from "lucide-preact";
import type {
  Report,
  TerminalControl,
  TerminalInfo,
  TerminalNotice,
} from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import { folderName } from "../../shared/folder-label.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { Button, IconButton, Spinner } from "../../shared/ui.tsx";
import { KeyBar, keepFocus, type BarKey } from "../../shared/key-bar.tsx";
import { coarsePointer } from "../../shared/layout.ts";
import { useLatest } from "../../shared/use-latest.ts";
import {
  maxTerminalChatChars,
  maxTerminals,
  reconnectBaseDelayMs,
  reconnectMaxDelayMs,
  terminalScrollbackLines,
} from "../../shared/limits.ts";

// Shells the host runs for this user, each an xterm.js view on a socket.
// The agent never sees a terminal; "Add to chat" puts text in the composer.

interface View {
  text: () => string;
  focus: () => void;
}
type ViewState = "connecting" | "live" | "replaced" | "ended";

// The 16 ANSI colours, by xterm's names; each is a --terminal-* token.
const ANSI = [
  "black",
  "red",
  "green",
  "yellow",
  "blue",
  "magenta",
  "cyan",
  "white",
  "brightBlack",
  "brightRed",
  "brightGreen",
  "brightYellow",
  "brightBlue",
  "brightMagenta",
  "brightCyan",
  "brightWhite",
] as const;
const APPLE = /Mac|iPhone|iPad/.test(navigator.platform);
const kebab = (name: string) =>
  name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`);

function lookOf(element: Element) {
  const style = getComputedStyle(element);
  const token = (name: string) => style.getPropertyValue(`--${name}`).trim();
  const theme: ITheme = {
    background: token("bg"),
    foreground: token("text"),
    cursor: token("accent"),
    cursorAccent: token("bg"),
    selectionBackground: token("placeholder"),
  };
  for (const color of ANSI) theme[color] = token(`terminal-${kebab(color)}`);
  return {
    theme,
    fontFamily: style.fontFamily,
    fontSize: Number.parseFloat(style.fontSize),
  };
}

// Arrows follow the cursor mode a full-screen program sets.
function sequence(key: BarKey, application: boolean) {
  const arrow = application ? "\x1bO" : "\x1b[";
  return {
    Escape: "\x1b",
    Tab: "\t",
    ArrowUp: `${arrow}A`,
    ArrowDown: `${arrow}B`,
    ArrowRight: `${arrow}C`,
    ArrowLeft: `${arrow}D`,
  }[key];
}

// The end of the text, fenced longer than any run of backticks inside.
function fenced(text: string) {
  const kept = text.slice(-maxTerminalChatChars);
  const longest = Math.max(
    0,
    ...(kept.match(/`+/g) || []).map((run) => run.length),
  );
  const fence = "`".repeat(Math.max(3, longest + 1));
  return `${fence}console\n${kept}\n${fence}`;
}

// Where a shell runs: the home folder ("") is ~.
const where = (cwd: string) => (cwd ? folderName(cwd) : "~");

// The socket reattaches until the shell ends or another device takes it.
function TerminalView({
  id,
  active = true,
  register = () => {},
  ended = () => {},
}: {
  id: string;
  active?: boolean;
  register?: (id: string, view: View | null) => void;
  ended?: (id: string) => void;
}) {
  const { routes } = useApi();
  const box = useRef<HTMLDivElement>(null);
  const terminal = useRef<Terminal | null>(null);
  const fit = useRef<FitAddon | null>(null);
  const socket = useRef<WebSocket | null>(null);
  const [state, setState] = useState<ViewState>("connecting");
  const [code, setCode] = useState(-1);
  // Bumped to take the shell back from another device.
  const [attempt, setAttempt] = useState(0);
  const [ctrl, setCtrl] = useState(false);
  const [typing, setTyping] = useState(false);
  const send = (data: string | Uint8Array<ArrayBuffer>) => {
    if (socket.current?.readyState === WebSocket.OPEN)
      socket.current.send(data);
  };
  // What a typed key sends: with a held Ctrl, a letter or @[\]^_ is that
  // character's control code.
  const typed = (data: string) => {
    if (!latest.current.ctrl || data.length !== 1) return data;
    setCtrl(false);
    const letter = data.toUpperCase().charCodeAt(0);
    return letter >= 64 && letter < 96
      ? String.fromCharCode(letter - 64)
      : data;
  };
  const latest = useLatest({ ctrl, ended, register, send, typed });

  useEffect(() => {
    const element = box.current;
    if (!element) return;
    const look = lookOf(element);
    const term = new Terminal({
      ...look,
      cursorBlink: true,
      scrollback: terminalScrollbackLines,
    });
    const fitting = new FitAddon();
    term.loadAddon(fitting);
    term.loadAddon(new WebLinksAddon());
    // The panel's own shortcut (Ctrl+`, ⌘ too on a Mac: shared/shortcuts.ts)
    // reaches the page, not the shell. Spelled out here, as importing the
    // table would move it out of the first-load bundle.
    term.attachCustomKeyEventHandler(
      (event) =>
        !(
          event.type === "keydown" &&
          event.key === "`" &&
          (event.ctrlKey || (APPLE && event.metaKey))
        ),
    );
    term.open(element);
    term.textarea?.addEventListener("focus", () => setTyping(true));
    term.textarea?.addEventListener("blur", () => setTyping(false));
    terminal.current = term;
    fit.current = fitting;
    const encoder = new TextEncoder();
    const keyed = term.onData((data) =>
      latest.current.send(encoder.encode(latest.current.typed(data))),
    );
    // Some mouse modes report raw bytes.
    const binary = term.onBinary((data) =>
      latest.current.send(Uint8Array.from(data, (char) => char.charCodeAt(0))),
    );
    const resized = term.onResize(({ rows, cols }) => {
      const control: TerminalControl = { type: "resize", rows, cols };
      latest.current.send(JSON.stringify(control));
    });
    const observer = new ResizeObserver(() => {
      if (element.offsetParent) fitting.fit();
    });
    observer.observe(element);
    // A theme or zoom change redraws it in the new tokens.
    let drawn = JSON.stringify(look);
    const restyle = new MutationObserver(() => {
      const next = lookOf(element);
      if (JSON.stringify(next) === drawn) return;
      drawn = JSON.stringify(next);
      term.options.theme = next.theme;
      term.options.fontFamily = next.fontFamily;
      term.options.fontSize = next.fontSize;
      if (element.offsetParent) fitting.fit();
    });
    restyle.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ["data-theme", "style"],
    });
    latest.current.register(id, {
      text: () => {
        const selected = term.getSelection();
        if (selected) return selected;
        const buffer = term.buffer.active;
        const lines = [];
        for (let row = 0; row < term.rows; row++)
          lines.push(
            buffer.getLine(buffer.viewportY + row)?.translateToString(true) ||
              "",
          );
        return lines.join("\n").trimEnd();
      },
      focus: () => term.focus(),
    });
    return () => {
      latest.current.register(id, null);
      observer.disconnect();
      restyle.disconnect();
      keyed.dispose();
      binary.dispose();
      resized.dispose();
      term.dispose();
      terminal.current = null;
      fit.current = null;
    };
  }, [id]);

  useEffect(() => {
    let delay = reconnectBaseDelayMs;
    let timer: ReturnType<typeof setTimeout> | undefined;
    // The shell ended, another device took it or the view went away: no
    // more reconnecting.
    let settled = false;
    const connect = () => {
      const url = new URL(routes.terminal(id), location.href);
      url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
      const next = new WebSocket(url);
      next.binaryType = "arraybuffer";
      socket.current = next;
      next.onopen = () => {
        delay = reconnectBaseDelayMs;
        setState("live");
        const term = terminal.current;
        if (!term) return;
        // The host replays what the shell kept.
        term.reset();
        if (box.current?.offsetParent) fit.current?.fit();
        const control: TerminalControl = {
          type: "resize",
          rows: term.rows,
          cols: term.cols,
        };
        next.send(JSON.stringify(control));
      };
      next.onmessage = (event: MessageEvent<ArrayBuffer | string>) => {
        if (typeof event.data !== "string") {
          terminal.current?.write(new Uint8Array(event.data));
          return;
        }
        const notice: TerminalNotice = JSON.parse(event.data);
        settled = true;
        if (notice.type === "exit") {
          setCode(notice.code);
          setState("ended");
          latest.current.ended(id);
        } else setState("replaced");
      };
      next.onclose = () => {
        if (settled) return;
        setState("connecting");
        timer = setTimeout(connect, delay);
        delay = Math.min(delay * 2, reconnectMaxDelayMs);
      };
    };
    connect();
    return () => {
      settled = true;
      clearTimeout(timer);
      socket.current?.close();
      socket.current = null;
    };
  }, [id, attempt, routes.terminal]);

  // A tab shown again takes the room it now has, and the keys.
  useEffect(() => {
    if (!active) return;
    if (box.current?.offsetParent) fit.current?.fit();
    if (!coarsePointer()) terminal.current?.focus();
  }, [active]);

  return (
    <div class="terminal-tabpanel" id={`terminal-${id}`} hidden={!active}>
      <div class="terminal-view" ref={box} />
      {state !== "live" && (
        <p class="terminal-note small" role="status">
          {state === "connecting" ? (
            "Reconnecting…"
          ) : state === "ended" ? (
            code < 0 ? (
              "The shell ended."
            ) : (
              `The shell exited with code ${code}.`
            )
          ) : (
            <>
              Open on another device.{" "}
              <Button variant="quiet" onClick={() => setAttempt(attempt + 1)}>
                Use here
              </Button>
            </>
          )}
        </p>
      )}
      {active && coarsePointer() && (
        <div class="terminal-keys">
          <KeyBar
            press={(key) => {
              const term = terminal.current;
              if (term)
                send(sequence(key, term.modes.applicationCursorKeysMode));
              setCtrl(false);
            }}
            ctrl={ctrl}
            setCtrl={setCtrl}
          >
            <Button
              aria-label={typing ? "Hide the keyboard" : "Show the keyboard"}
              onPointerDown={keepFocus}
              onClick={() =>
                typing
                  ? terminal.current?.textarea?.blur()
                  : terminal.current?.focus()
              }
            >
              <Keyboard />
            </Button>
          </KeyBar>
        </div>
      )}
    </div>
  );
}

function Place({
  name,
  cwd,
  ended = false,
}: {
  name: string;
  cwd: string;
  ended?: boolean;
}) {
  return (
    <span class="terminal-place" title={cwd}>
      {name}
      <span class="muted"> · {ended ? "ended" : where(cwd)}</span>
    </span>
  );
}

// One of the page's own shells, which has no start time. Its number is the
// lowest free one when it opened and stays for its life.
type PageTerminal = Omit<TerminalInfo, "title" | "started"> & {
  number: number;
};

// The shell runs on when this page leaves it.
export function FolderTerminal({
  terminal,
  close,
}: {
  terminal: TerminalInfo;
  close: (terminal: TerminalInfo) => void;
}) {
  return (
    <section
      class="terminal-panel"
      aria-label={`Terminal in ${where(terminal.cwd)}`}
    >
      <div class="terminal-bar">
        <Place name={terminal.title} cwd={terminal.cwd} />
        <Button onClick={() => close(terminal)}>Close terminal</Button>
      </div>
      <TerminalView key={terminal.id} id={terminal.id} />
    </section>
  );
}

// Closing the panel or the page ends these shells.
export default function TerminalPanel({
  cwd,
  addToChat,
  close,
  report,
}: {
  // Where a new terminal starts; "" is the home folder.
  cwd: string;
  addToChat?: (text: string) => void;
  close: () => void;
  report: Report;
}) {
  const { manage } = useApi();
  const [tabs, setTabs] = useState<PageTerminal[]>([]);
  const [active, setActive] = useState("");
  const [ended, setEnded] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const views = useRef(new Map<string, View>());

  const open = async () => {
    setBusy(true);
    try {
      const { id } = await manage("terminal", { action: "open", cwd });
      if (!id) return;
      setTabs((current) => {
        let number = 1;
        while (current.some((tab) => tab.number === number)) number++;
        return [...current, { id, cwd, folder: "", number }];
      });
      setActive(id);
    } catch (error) {
      report(error);
    } finally {
      setBusy(false);
    }
  };
  const remove = async (id: string) => {
    if (!ended.includes(id)) {
      try {
        await manage("terminal", { action: "close", id });
      } catch (error) {
        report(error);
        return;
      }
    }
    const kept = tabs.filter((tab) => tab.id !== id);
    setTabs(kept);
    if (!kept.length) close();
    else if (active === id) setActive(kept[kept.length - 1].id);
  };

  // biome-ignore lint/correctness/useExhaustiveDependencies: runs once
  useEffect(() => {
    void open();
  }, []);

  const current = tabs.find((tab) => tab.id === active);
  if (!tabs.length && busy) return <Spinner label="Opening the terminal…" />;
  return (
    <div class="terminal-panel">
      <div class="terminal-bar">
        <div class="terminal-tabs" role="group" aria-label="Terminals">
          {tabs.map((tab) => (
            <div class="terminal-tab entry" key={tab.id}>
              <Button
                variant="quiet"
                aria-current={tab.id === active ? "true" : undefined}
                aria-controls={`terminal-${tab.id}`}
                onClick={() => {
                  setActive(tab.id);
                  views.current.get(tab.id)?.focus();
                }}
              >
                <Place
                  name={`Terminal ${tab.number}`}
                  cwd={tab.cwd}
                  ended={ended.includes(tab.id)}
                />
              </Button>
              <IconButton
                label={`Close Terminal ${tab.number}`}
                onClick={() => void remove(tab.id)}
              >
                <X />
              </IconButton>
            </div>
          ))}
        </div>
        <IconButton
          label="New terminal"
          disabled={busy || tabs.length >= maxTerminals}
          onClick={() => void open()}
        >
          <Plus />
        </IconButton>
        <Menu label="Terminal menu">
          {addToChat && (
            <MenuItem
              disabled={!current}
              title="Put the selection, or what the terminal shows, into the message"
              onClick={() => {
                const text = views.current.get(active)?.text();
                if (text) addToChat(fenced(text));
              }}
            >
              Add to chat
            </MenuItem>
          )}
          <MenuItem disabled={!current} onClick={() => void remove(active)}>
            Close terminal
          </MenuItem>
        </Menu>
        <IconButton label="Hide the terminal" onClick={close}>
          <ChevronDown />
        </IconButton>
      </div>
      {tabs.map((tab) => (
        <TerminalView
          key={tab.id}
          id={tab.id}
          active={tab.id === active}
          register={(id, view) => {
            if (view) views.current.set(id, view);
            else views.current.delete(id);
          }}
          ended={(id) => setEnded((list) => [...list, id])}
        />
      ))}
      {!tabs.length && (
        <p class="terminal-note small">No terminal is running.</p>
      )}
    </div>
  );
}
