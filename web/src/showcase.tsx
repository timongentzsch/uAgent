import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Check, Copy, Plus } from "lucide-preact";
import {
  Button,
  Field,
  IconButton,
  Mark,
  Modal,
  Select,
  Skeleton,
  Spinner,
  Toggle,
} from "./shared/ui.tsx";
import { Menu, MenuItem } from "./shared/popover.tsx";
import { applyTheme } from "./shared/layout.ts";
import {
  applyZoom,
  normalizeZoom,
  SizeControls,
} from "./shared/size-controls.tsx";
import { readStored, writeStored } from "./state/store.ts";
import BrowserInput from "./features/browser/input.tsx";
import "./shared/style.css";
import "./features/browser/browser.css";
import "./showcase.css";

function BrowserInputSample() {
  const screen = useRef<HTMLDivElement>(null);
  const target = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!target.current || target.current.querySelector("canvas")) return;
    const canvas = document.createElement("canvas");
    canvas.width = 800;
    canvas.height = 500;
    canvas.style.width = "100%";
    canvas.style.height = "100%";
    const context = canvas.getContext("2d")!;
    context.fillStyle = "#e2e2e2";
    context.fillRect(0, 0, canvas.width, canvas.height);
    context.fillStyle = "#171717";
    context.font = "24px monospace";
    for (let y = 40; y < canvas.height; y += 80)
      for (let x = 20; x < canvas.width; x += 160)
        context.fillText(`${x},${y}`, x, y);
    target.current.append(canvas);
  }, []);
  return (
    <div class="showcase-browser-input">
      <BrowserInput
        screen={screen}
        target={target}
        disabled={false}
        showTrackpad
        readOnly={false}
      />
    </div>
  );
}

const DEMO_LOAD_DELAY_MS = 800;
function DelayedContent() {
  const [ready, setReady] = useState(false);
  useEffect(() => {
    const timer = setTimeout(() => setReady(true), DEMO_LOAD_DELAY_MS);
    return () => clearTimeout(timer);
  }, []);
  return ready ? (
    <div class="settings-fields">
      <Field label="Loaded value">
        <input defaultValue="Content arrived without resizing the shell" />
      </Field>
      <p>
        This example deliberately delays content so layout changes are easy to
        inspect.
      </p>
    </div>
  ) : (
    <Spinner label="Loading example…" surface />
  );
}

function Showcase() {
  const [theme, setTheme] = useState(
    () => localStorage.getItem("uagent-theme") || "system",
  );
  const [zoom, setZoom] = useState(() =>
    normalizeZoom(readStored<number>(localStorage, "uagent-zoom", 100)),
  );
  const [enabled, setEnabled] = useState(true);
  const [dialog, setDialog] = useState<
    "example" | "browser" | "loading" | null
  >(null);

  useEffect(() => applyTheme(theme), [theme]);
  useEffect(() => {
    writeStored(localStorage, "uagent-zoom", zoom);
    applyZoom(zoom);
  }, [zoom]);

  return (
    <main class="showcase">
      <header class="showcase-head">
        <div class="showcase-title">
          <Mark />
          <div>
            <h1>µAgent UI showcase</h1>
            <p>Shared tokens, controls, states, and surfaces.</p>
          </div>
        </div>
        <a class="button-link" href="/">
          Back to workspace
        </a>
      </header>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Foundations</h2>
            <p>Theme and density use the same settings as the app.</p>
          </div>
        </div>
        <div class="showcase-grid showcase-grid-two">
          <div class="showcase-card">
            <Field label="Appearance">
              <Select
                aria-label="Appearance"
                value={theme}
                onChange={(event) => setTheme(event.currentTarget.value)}
              >
                <option value="system">System</option>
                <option value="dark">Dark</option>
                <option value="light">Light</option>
              </Select>
            </Field>
          </div>
          <div class="showcase-card">
            <SizeControls zoom={zoom} change={setZoom} />
          </div>
        </div>
        <div class="token-grid" aria-label="Color tokens">
          {[
            ["Background", "bg"],
            ["Surface", "surface"],
            ["Raised", "raised"],
            ["Text", "text"],
            ["Muted", "muted"],
            ["Accent", "accent"],
          ].map(([label, token]) => (
            <div class="token" key={token}>
              <span style={`background:var(--${token})`} />
              <small>{label}</small>
            </div>
          ))}
        </div>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Actions</h2>
            <p>Solid primary actions and flat raised secondary actions.</p>
          </div>
        </div>
        <div class="showcase-grid showcase-grid-two">
          <div class="showcase-card">
            <h3>Variants</h3>
            <div class="showcase-row">
              <Button variant="primary">Primary action</Button>
              <Button>Secondary action</Button>
              <Button variant="quiet">Quiet action</Button>
              <Button variant="destructive">Destructive action</Button>
            </div>
          </div>
          <div class="showcase-card">
            <h3>Sizes and icons</h3>
            <div class="showcase-row">
              <Button size="compact">Compact action</Button>
              <Button class="with-icon">
                <Plus aria-hidden="true" />
                With icon
              </Button>
              <IconButton label="Copy example">
                <Copy aria-hidden="true" />
              </IconButton>
              <Menu label="Example menu">
                <MenuItem>First action</MenuItem>
                <MenuItem>Second action</MenuItem>
                <MenuItem disabled>Unavailable</MenuItem>
              </Menu>
            </div>
          </div>
          <div class="showcase-card">
            <h3>States</h3>
            <div class="showcase-row">
              <Button disabled>Disabled</Button>
              <Button busy>Working</Button>
              <Button aria-pressed="true" class="with-icon">
                <Check aria-hidden="true" />
                Selected state
              </Button>
            </div>
            <p class="muted">
              Use Tab to inspect keyboard focus; touch uses pressed feedback.
            </p>
          </div>
        </div>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Inputs</h2>
            <p>
              Every field uses the same height, fill, spacing, and focus ring.
            </p>
          </div>
        </div>
        <div class="showcase-grid showcase-grid-two">
          <Field label="Text" help="Short supporting text belongs below.">
            <input defaultValue="Editable value" />
          </Field>
          <Field label="Selection">
            <Select defaultValue="balanced">
              <option value="fast">Fast</option>
              <option value="balanced">Balanced</option>
              <option value="thorough">Thorough</option>
            </Select>
          </Field>
          <Field label="Search">
            <input type="search" placeholder="Find something…" />
          </Field>
          <Toggle
            label="Example setting"
            help="A short description of the resulting behavior."
            checked={enabled}
            onChange={(event) => setEnabled(event.currentTarget.checked)}
          />
          <Field label="Long text">
            <textarea rows={4} defaultValue="Multiline content" />
          </Field>
        </div>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Feedback</h2>
            <p>
              Spinners cover indeterminate work; skeletons reserve known rows.
            </p>
          </div>
        </div>
        <div class="showcase-grid showcase-grid-two">
          <div class="showcase-card">
            <Spinner label="Loading conversation…" />
          </div>
          <div class="showcase-card">
            <Skeleton label="Loading known rows…" rows={3} delayMs={0} />
          </div>
          <div class="update-banner showcase-banner" role="status">
            <span>Update available with the latest fixes.</span>
            <button class="primary">Refresh now</button>
          </div>
          <div class="showcase-card status-sample">
            <span class="status-led active" aria-hidden="true" />
            Connected
          </div>
        </div>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Surfaces</h2>
            <p>Dialogs and menus stay square, bordered, and shadow free.</p>
          </div>
        </div>
        <div class="showcase-row">
          <Button onClick={() => setDialog("example")}>Open dialog</Button>
          <Button onClick={() => setDialog("loading")}>
            Open loading dialog
          </Button>
        </div>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Remote browser input</h2>
            <p>View gestures and relative pointer controls remain separate.</p>
          </div>
        </div>
        <Button onClick={() => setDialog("browser")}>Open browser input</Button>
      </section>

      {dialog === "example" && (
        <Modal title="Example dialog" close={() => setDialog(null)}>
          <p>
            A modal uses the same controls and spacing tokens as every page.
          </p>
          <div class="dialog-actions">
            <button onClick={() => setDialog(null)}>Cancel</button>
            <button class="primary" onClick={() => setDialog(null)}>
              Confirm
            </button>
          </div>
        </Modal>
      )}
      {dialog === "loading" && (
        <Modal
          title="Loading example"
          size="medium"
          layout="panel"
          close={() => setDialog(null)}
        >
          <DelayedContent />
        </Modal>
      )}
      {dialog === "browser" && (
        <Modal
          title="Browser input"
          size="browser"
          layout="panel"
          close={() => setDialog(null)}
        >
          <BrowserInputSample />
        </Modal>
      )}
    </main>
  );
}

render(<Showcase />, document.getElementById("app")!);
