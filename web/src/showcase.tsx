import { storage } from "./shared/storage.ts";
import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Check, Copy, Plus, Wrench } from "lucide-preact";
import {
  Actions,
  Button,
  Field,
  IconButton,
  Mark,
  Modal,
  Select,
  Skeleton,
  Spinner,
  Group,
  Row,
  SettingRow,
  Switch,
  Input,
  Textarea,
  ValueSelect,
  SectionTitle,
  EmptyState,
  LoadError,
  EventRow,
  DisclosureRow,
  Time,
  CodeCopy,
} from "./shared/ui.tsx";
import { Menu, MenuItem, Popover } from "./shared/popover.tsx";
import { ConnectionStatus, StatusLed } from "./shared/connection-status.tsx";
import { applyTheme, applyZoom, normalizeZoom } from "./shared/layout.ts";
import { ZoomSlider } from "./shared/zoom-slider.tsx";
import { readStored, writeStored } from "./state/store.ts";
import BrowserTouch from "./features/browser/touch.tsx";
import { BrowserFrame, BrowserTools } from "./features/browser/frame.tsx";
import { ImageViewerDialog } from "./shared/attachments.tsx";
import "./features/composer/attachments.css";
import "./features/chat/message.css";
import "./shared/style.css";
import "./showcase.css";

// A 400x300 image: small enough that a large screen shows it unscaled.
const SAMPLE_IMAGE = `data:image/svg+xml,${encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">' +
    '<rect width="400" height="300" fill="#3b82f6"/>' +
    '<circle cx="200" cy="150" r="80" fill="#fff"/></svg>',
)}`;

function BrowserInputSample() {
  const screen = useRef<HTMLDivElement>(null);
  const target = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!target.current || target.current.querySelector("canvas")) return;
    const canvas = document.createElement("canvas");
    canvas.width = 800;
    canvas.height = 500;
    canvas.style.width = "100%";
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
    <BrowserFrame
      screen={
        <BrowserTouch
          screen={screen}
          target={target}
          label="Browser viewport"
          // No connection here: record what a server would receive.
          pointer={(x, y, mask) =>
            ((globalThis as { browserPointer?: number[][] }).browserPointer ??=
              []).push([Math.round(x), Math.round(y), mask])
          }
        />
      }
      bar={
        <>
          <div class="popover-control browser-status-menu">
            <Button variant="quiet" class="with-icon browser-status">
              <span>Driving</span>
            </Button>
          </div>
          <BrowserTools disabled={false} />
          <Button variant="primary" class="browser-primary">
            Done
          </Button>
        </>
      }
    />
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
        <Input defaultValue="Content arrived without resizing the shell" />
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
    () => storage.getItem("uagent-theme") || "system",
  );
  const [zoom, setZoom] = useState(() =>
    normalizeZoom(readStored<number>(storage, "uagent-zoom", 100)),
  );
  const [enabled, setEnabled] = useState(true);
  const [dialog, setDialog] = useState<
    "example" | "browser" | "loading" | "image" | null
  >(null);

  useEffect(() => applyTheme(theme), [theme]);
  useEffect(() => {
    writeStored(storage, "uagent-zoom", zoom);
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
            <Group>
              <SettingRow
                name="Zoom"
                htmlFor="zoom"
                detail="A changed setting offers Reset."
                overridden={zoom !== 100}
                reset={() => setZoom(100)}
              >
                <ZoomSlider zoom={zoom} change={setZoom} />
              </SettingRow>
              <SettingRow
                name="Locked setting"
                locked="Set by the environment; change it there."
                overridden
                reset={() => {}}
              >
                <Switch
                  label="Locked setting"
                  checked
                  disabled
                  onChange={() => {}}
                />
              </SettingRow>
            </Group>
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
              <Popover
                label="Example popover"
                trigger={<Wrench aria-hidden="true" />}
              >
                <p>Anchored panel content.</p>
              </Popover>
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
            <Input defaultValue="Editable value" />
          </Field>
          <Field label="Selection">
            <Select defaultValue="balanced">
              <option value="fast">Fast</option>
              <option value="balanced">Balanced</option>
              <option value="thorough">Thorough</option>
            </Select>
          </Field>
          <Field label="Search">
            <Input type="search" placeholder="Find something…" />
          </Field>
          <Field label="Number">
            <Input type="number" defaultValue="42" />
          </Field>
          <Field label="Password">
            <Input type="password" defaultValue="example" />
          </Field>
          <Field label="Date and time">
            <Input type="datetime-local" defaultValue="2026-09-24T12:00" />
          </Field>
          <Field label="Long text">
            <Textarea rows={4} defaultValue="Multiline content" />
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
            <Button variant="primary">Refresh now</Button>
          </div>
          <div class="showcase-card status-sample">
            <ConnectionStatus phase="connected" />
            <ConnectionStatus phase="reconnecting" />
            <span>
              <StatusLed state="running" /> Running ·{" "}
              <Time value={Date.now()} />
            </span>
            <span>
              <StatusLed state="failed" /> Failed
            </span>
          </div>
          <div class="showcase-card">
            <LoadError
              error={new Error("The host did not answer.")}
              retry={() => {}}
            />
          </div>
          <div class="showcase-card">
            Copy feedback <CodeCopy text="example" />
          </div>
        </div>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Lists and rows</h2>
            <p>
              Flat grouped rows carry a value, a switch or a destination;
              disclosures reveal detail in place.
            </p>
          </div>
        </div>
        <div class="showcase-grid showcase-grid-two">
          <div>
            <Group
              title="Grouped rows"
              footer="Help text belongs below a group."
            >
              <Row
                label="Example setting"
                detail="A short description of the resulting behavior."
              >
                <Switch
                  label="Example setting"
                  checked={enabled}
                  onChange={setEnabled}
                />
              </Row>
              <Row label="Choice">
                <ValueSelect aria-label="Choice" value="auto">
                  <option value="ask">Ask</option>
                  <option value="auto">Auto</option>
                </ValueSelect>
              </Row>
              <Row label="Destination" onClick={() => {}} />
              <Row
                label="Expandable"
                detail="Opens its detail below the row."
                expanded
                onClick={() => {}}
              />
              <Row label="Remove this device" destructive onClick={() => {}} />
            </Group>
            <SectionTitle>Section title</SectionTitle>
            <EmptyState action={<Button>Create one</Button>}>
              Nothing here yet.
            </EmptyState>
          </div>
          <div class="showcase-card">
            <DisclosureRow label="Disclosure row" status="done">
              <p>Detail revealed in place.</p>
            </DisclosureRow>
            <EventRow title="Event row" icon={<Check aria-hidden="true" />}>
              <p>What happened, in detail.</p>
            </EventRow>
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
            <p>Tap clicks, drag scrolls, hold-drag drags, pinch zooms.</p>
          </div>
        </div>
        <Button onClick={() => setDialog("browser")}>Open browser input</Button>
      </section>

      <section class="showcase-section">
        <div class="showcase-section-head">
          <div>
            <h2>Image viewer</h2>
            <p>Full bleed; pinch, double-tap or Ctrl+scroll to zoom.</p>
          </div>
        </div>
        <Button onClick={() => setDialog("image")}>Open image viewer</Button>
      </section>

      {dialog === "example" && (
        <Modal title="Example dialog" close={() => setDialog(null)}>
          <p>
            A modal uses the same controls and spacing tokens as every page.
          </p>
          <Actions>
            <Button onClick={() => setDialog(null)}>Cancel</Button>
            <Button variant="primary" onClick={() => setDialog(null)}>
              Confirm
            </Button>
          </Actions>
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
      {dialog === "image" && (
        <ImageViewerDialog
          image={{ src: SAMPLE_IMAGE, name: "sample.svg" }}
          close={() => setDialog(null)}
        />
      )}
      {dialog === "browser" && (
        <Modal
          title="Browser input"
          className="browser-view"
          size="browser"
          layout="sheet"
          close={() => setDialog(null)}
        >
          <BrowserInputSample />
        </Modal>
      )}
    </main>
  );
}

render(<Showcase />, document.getElementById("app")!);
