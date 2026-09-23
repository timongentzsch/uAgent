# Web UI audit and refactor

Reviewed 2026-09-23. Scope: app shell, shared controls and CSS, conversation and
child history, composer, settings/library/schedules, browser input, client state,
SSE delivery, Markdown and PWA lifecycle. Native changes are limited to child
inspection and follow-up dispatch. This is not a claim that every native path or
physical mobile device has been exercised.

## High-value changes

| Area | Finding | Shared implementation |
| --- | --- | --- |
| Browser cursor | noVNC's fallback cursor lives under `body`, behind modal dialogs. Outer CSS scaling also bypassed noVNC's coordinate conversion. | Cursor overlay inside the dialog; one framebuffer position for drawing and input; resize the noVNC container for zoom; pan to reveal the pointer. At edges the arrow points inward. |
| Touch controls | Unconditional hover styles could remain active after a tap. | Gate hover on hover/fine-pointer capability. Keep focus and pressed states available. Preserve the existing taller trackpad and equal mouse buttons. |
| Dialog loading | Lazy CSS and content determined initial dimensions; a two-frame opacity delay could not guarantee settled data. | `Modal` owns size and panel height in eager CSS. Browser/settings retain one spinner while loading. Content scrolls within the stable shell. |
| Subagent parity | History was shared, but statistics callbacks, model controls and input behavior diverged. Live worker state was reduced to its transcript. | Reuse `StatisticsContent`, `SessionSummary`, `ModelControl`, `MessageInput`, `MessageRows` and `useTranscriptHistory`. Pass an optional follow-up model through the existing native subagent tool. |
| Loaded history | New retained rows silently trimmed explicitly loaded pages to 256 entries. | Preserve loaded history. The separate cache still evicts inactive session views. |
| Async work | Browser polling could overlap; read requests and PWA listeners could outlive their surface. | Serialize status requests, suspend hidden polling, abort superseded inspection reads, and clean up service-worker listeners. |
| Feature boundaries | Activity imported a transport helper from settings UI, pulling unrelated styles into its dependency graph. | `state/api.ts` owns command/management transport. Feature modules own presentation. |
| Markdown rendering | A progressive prefix could advance before its formatted blocks were ready, temporarily dropping text and moving the reader. Concurrent preparation also double-counted cache entries. | Commit the prefix and blocks together; derive the remaining plain text from that frame. Count unique cache entries and ignore late work after unmount. |
| Design review | Showcase examples did not consistently use production controls. | Showcase uses shared button variants, modal loading and actual browser input in a modal. |
| Auto permissions | Settings described the default reviewer by model name. | Product copy describes Auto review; the actual reviewer remains configurable. |

## Conversation parity contract

Main and child views use the same turn/session rows, cost formatting, missing-data
labels and scope selector. Child totals belong to that child and its descendants;
the parent totals are not copied into the child. Native counters remain the
authority. Parent and side durations stay separate because work can overlap.

Persistent workers expose available live counters. Process children can expose
only a saved checkpoint while running; the statistics panel explicitly says so.
Missing provider costs stay unreported. See [Measurements](MEASUREMENTS.md).

Ordinary child follow-ups can choose model, variant and effort without changing
the parent selection. Child launches explicitly select the resolved model so a
restored journal cannot overwrite that choice. Persistent agents keep their
existing runtime model; the UI explains that restriction. Guidance during a
running turn keeps the active model. Both input fields share Enter/Shift+Enter,
IME handling and automatic height. Session-specific submission, attachments
and approval ownership remain with their existing controllers.

## Architecture and performance

The current core already has the right ownership: one ordered native stream,
one browser state reducer, frame-batched presentation, retained messages, shared
history anchoring and lazy renderers. Replacing those mechanisms would increase
the regression surface. This refactor extends them and removes the duplicate
paths listed above.

The synthetic browser workload in `web/tests/performance.spec.js` supplies 5,000
mock word tokens over five seconds through the actual EventSource handlers. It
checks lossless final text, samples frame gaps and types while streaming.
This measures browser processing, not provider tokenization, network throughput
or performance on an iPhone. The native transport separately coalesces text and
usage events; its policies are documented in [Measurements](MEASUREMENTS.md).

Local Chromium samples (one run each, not a statistical speedup claim):

| Measurement | Previous master | Refactor |
| --- | ---: | ---: |
| Mock tokens / elapsed | 5,000 / 5,002 ms | 5,000 / 5,002 ms |
| Frame gap, 95th percentile | 16.8 ms | 16.7 ms |
| Longest sampled frame gap | 16.8 ms | 16.8 ms |
| Input round trip during streaming | Not sampled | 11.5 ms |
| Input round trip after streaming | 25.9 ms | 8.4 ms |

These runs exercised a 25,000-character plain response on this development
machine. They establish no observed browser stall for that workload, not a
guarantee for long Markdown, simultaneous children or lower-powered devices.

Cache and rendering budgets live in `web/src/shared/limits.ts`; gesture tuning
lives in `features/browser/gestures.ts`. These are policies, not measured device
limits. Keep bundle measurements advisory. Reduce dependencies or work based on
observed cost, not an arbitrary line count.

Build sizes (gzip bytes): initial JavaScript 29,369 → 28,987; initial CSS
5,384 → 5,562; precache 146,015 → 147,259. Added parity and layout behavior
therefore fits within essentially the same delivery footprint. The full asset
set is 2,051,273 gzip bytes, with 1,893,250 bytes loaded only for optional
rendering/viewer features.

## Remaining targets and evidence required

- Very long loaded histories: profile DOM count and input latency on a phone
  before introducing virtualization. Any windowing must preserve selection,
  disclosure state and the existing history anchor contract.
- Large diagrams: Mermaid is the dominant optional asset. Keep it lazy and
  bounded. A replacement needs equivalent rendering/security behavior and
  measured startup or memory benefit.
- Background process children: real-time statistics parity requires a native
  child-state subscription. Saved checkpoints are labeled honestly meanwhile;
  another UI polling loop would duplicate work.
- Persistent child model changes: require an explicit runtime lifecycle design;
  a disabled picker must not imply that a new model was applied.
- Physical iOS PWA validation remains necessary for OS keyboard/focus zoom and
  touch ergonomics. Automated Chromium/WebKit tests cover computed font sizes,
  viewport bounds, touch capability, cursor geometry and real noVNC encoding.

## Design references

- [shadcn button variants and sizes](https://ui.shadcn.com/docs/components/base/button):
  use a small shared variant API and production controls in the showcase.
- [Tailwind hover behavior](https://tailwindcss.com/docs/upgrade-guide#hover-styles-on-mobile):
  apply hover effects only when the primary input supports hovering.
- [MDN top layer](https://developer.mozilla.org/en-US/docs/Glossary/Top_layer):
  modal overlays and their cursor must share the appropriate browser layer.
- [noVNC API](https://novnc.com/noVNC/docs/API.html): keep transport/display
  ownership in noVNC and avoid private client input methods.
