// The native host owns this versioned wire contract. Unknown JSON stays at the
// HTTP boundary; components share these projections instead of redefining them.
import type { ArtifactType } from "./artifact.ts";

export type JSONValue =
  | null
  | boolean
  | number
  | string
  | JSONValue[]
  | { [key: string]: JSONValue };
// "inline": the surface that failed shows it; the banner stays clear.
export type Report = (error: unknown, scope?: "inline") => void;
interface Failure extends Error {
  // The host's name for a refusal a client acts on (docs/interfaces/http-api.md).
  code?: string;
  network?: boolean;
  status?: number;
  rejected?: boolean;
  choices?: FileOrigin[];
}
export function failure(error: unknown): Failure {
  return error instanceof Error ? error : new Error(String(error));
}
// A refusal the host sent, with its code when it names one.
export const refusal = (message = "", code?: string): Failure =>
  Object.assign(new Error(message), { code });
export interface Asset {
  id: string;
  name: string;
  bytes: number;
  image?: boolean;
  pending?: boolean;
}
export interface Draft {
  text: string;
  files: Asset[];
  // Sent before the text, each a paragraph of its own.
  quotes: string[];
}
export interface InstallPrompt extends Event {
  prompt(): Promise<void>;
}
export interface Usage {
  input?: number;
  output?: number;
  reasoning?: number;
  cache_read?: number;
  cache_write?: number;
  cost: number;
  cost_reported?: boolean;
}
export interface Statistics {
  recorded_turns?: number;
  incoming?: number;
  complete?: boolean;
  tool_calls?: number;
  model_calls?: number;
  duration_ms?: number;
  model_ms?: number;
  tool_ms?: number;
  ttft_samples?: number;
  ttft_ms?: number;
  generation_ms?: number;
  generated_tokens?: number;
  side_recorded_turns?: number;
  side_tool_calls?: number;
  side_model_calls?: number;
  side_duration_ms?: number;
  // Per upstream a router such as OpenRouter chose: the model calls it
  // served and the tool calls from its responses that were rejected.
  served_by?: Record<string, { model_calls?: number; rejected_calls?: number }>;
}
export interface Exchange {
  id: string;
  state: string;
  purpose?: string;
  attempt?: number;
  status?: number;
  preview?: boolean;
  complete?: boolean;
  method?: string;
  url?: string;
  time?: string;
  request_headers?: string;
  response_headers?: string;
  served_by?: string;
  generation_id?: string;
}
// One state vocabulary for every call and receipt.
type RowState = "running" | "ok" | "failed" | "cancelled";
// What a finished call's result was. The host words it: `word` is the
// whole subtitle ("exit 1 · 2.3s"), absent when there is none; the facts
// behind it are for reading.
interface ResultFacts {
  word?: string;
  exit?: { code?: number; signal?: number };
  added?: number;
  removed?: number;
  items?: number;
  // On a turn's work: how many files it has changed, and how often it
  // thought.
  edited?: number;
  thoughts?: number;
  unit?: string;
  outcome?: string;
  reason?: string;
}
// How a row draws its body: a "tail" keeps a command's last lines visible.
export type BodyKind =
  | "text"
  | "markdown"
  | "tail"
  | "diff"
  | "image"
  | "link"
  | "fields";
// How a call reads, built natively by each tool (see ToolView in tool.h).
export type ToolPart =
  | RefusalPart
  | { kind: "command"; text: string }
  | {
      kind: "code";
      text?: string;
      first_line?: number;
      language?: string;
      label?: string;
    }
  | { kind: "fields"; rows: [string, string][] }
  | { kind: "source"; url: string; title?: string; excerpt?: string }
  | { kind: "markdown"; text: string }
  | FilePart
  | ArtifactPart
  | LinkPart;
// A call refused without asking: by the review, with the action to allow
// for this conversation, or by a rule of the folder.
export interface RefusalPart {
  kind: "refusal";
  source: "review" | "deny_rule";
  reason: string;
  rule?: string;
  offer?: { tool: string; match: string };
}
// Pages in the folder the agent presented: opened in Files, never copied.
export interface ArtifactPart {
  kind: "artifact";
  type: ArtifactType;
  // Its folder (`decks/q3/`) or its page; `cover` is the page shown small.
  path: string;
  cover: string;
  count: number;
}
export interface FilePart {
  kind: "file";
  id: string;
  name: string;
  mime: string;
  bytes: number;
  // A raster picture's size; old conversations lack it.
  width?: number;
  height?: number;
}
export interface LinkPart {
  kind: "link";
  to: "agent" | "activity" | "memory" | "browser" | "file";
  state?: string;
  report?: string;
  id: string | number;
  label: string;
}
// "Editing a.ts" while running, "Edited a.ts" after; input shows on expand.
interface ToolView {
  verb?: [string, string];
  target?: string;
  input?: ToolPart[];
}
interface TurnSummary {
  usage_reported?: boolean;
  turn: number;
  outcome: string;
  route?: string;
  tool_calls: number;
  model_calls?: number;
  background_statistics?: Statistics;
  steps: number;
  duration_ms: number;
  ttt_ms: number;
  tokens_per_second: number;
  usage: Usage;
  // Files the turn's edit, write and delete tools changed (at most 50).
  files?: TurnFile[];
  generation_ms?: number;
  generated_tokens?: number;
  messages?: number;
  context_tokens?: number;
}
export interface TurnFile {
  path: string;
  added: number;
  removed: number;
  undoable: boolean;
}
// Why the last turn ended; null while one runs.
interface TurnStop {
  reason: string;
  detail?: string;
}
// What a note about memory says, as the host recorded it.
interface MemoryNote {
  action: string;
  key: string;
  automatic: boolean;
}
// The agent a note or report came from: its conversation, its name, and why
// its turn ended; a decision request also names the tool it asks to run.
interface AgentFacts {
  session: string;
  title: string;
  outcome: string;
  tool?: string;
}
// One node of the transcript tree as the host sends it (include/agent/
// node.h): `parent` links it into the tree. A node with a headline is a row
// that opens to its body and children; the others are messages and a turn's
// footer. The fields beside the contract's are those its kind carries.
export interface Node {
  id: string;
  parent: string | null;
  kind:
    | "turn"
    | "message"
    | "work"
    | "group"
    | "call"
    | "thinking"
    | "notice"
    | "footer";
  sequence?: number;
  time?: string;
  state?: RowState;
  routine?: boolean;
  tool?: string;
  headline?: { verb: string; target: string };
  result?: ResultFacts;
  body?: { kind: BodyKind; text: string; truncated?: boolean; tail?: string };
  // A message's author: you (or what woke the conversation), or the agent.
  role?: "user" | "assistant";
  // Files a call read, or what wrote a message in your place: an agent's
  // report, finished background work, another conversation's message, or
  // the harness asking a side chat to sum itself up.
  origin?: "tool" | "agent" | "activity" | "message" | "merge";
  agent?: AgentFacts;
  request_id?: string;
  incoming?: number;
  streaming?: boolean;
  input?: ToolPart[];
  parts?: ToolPart[];
  files?: Asset[];
  deliveries?: Delivery[];
  unavailable_images?: number;
  // A reply's model call, and the HTTP capture of a turn's failed ones.
  http?: Exchange[];
  route?: string;
  served_by?: string;
  duration_ms?: number;
  ttft_ms?: number;
  tokens_per_second?: number;
  usage?: Usage;
  usage_reported?: boolean;
  memory?: MemoryNote;
  // On the row of a move: the folder the conversation came from.
  moved?: { from: string };
  activity_id?: number;
  command?: string;
  summary?: TurnSummary;
  // A message of yours this device holds until the host's own node
  // replaces it: where it is on its way (see outgoingStatus), and why the
  // host refused it.
  status?: string;
  error?: string;
}
// How an attachment reached the model.
interface Delivery {
  id: string;
  name: string;
  delivery: string;
  path: string;
}
export interface FileOrigin {
  cwd: string;
  host: string;
  origin: string;
  current: boolean;
}
// What the inspector opens: a row's own work, or the one a link names.
export interface ActivityTarget {
  id: string;
  label: string;
  state?: RowState;
  text?: string;
  command?: string;
  activity_id?: number;
  memory?: MemoryNote;
}
// A message of yours, held until the host confirms it.
export type Outgoing = Node & { request_id: string; session_id: string };
export interface View {
  nodes: Node[];
  before?: number;
  more?: boolean;
  dropped_segments?: number;
  fork?: { title: string; turns: number };
}
export interface Activity {
  id?: number;
  activity_id?: number;
  label?: string;
  command?: string;
  status?: string;
  started_ms?: number;
  duration_ms?: number;
  progress?: string;
  // A command the turn is still waiting on, until moved to the background.
  detached?: boolean;
}
// One item of a conversation's to-do list (the todo tool).
export interface TodoItem {
  text: string;
  status: "pending" | "done";
}
export interface ActivityDetail extends Activity {
  memory?: MemoryNote;
  output?: string;
}
interface PendingBase {
  id: string;
  prompt?: string;
  initial?: string;
  // Terminal text shown above the prompt; the web draws `options` instead.
  description?: string;
  options?: (string | { value: string; label?: string; title?: string })[];
  // An agent's decision goes to its parent first; "human" once yielded.
  route?: "parent" | "human";
  // The shared files it is about: plan mode's plan.
  files?: FilePart[];
  note?: string;
}
// The model's questions; `prompt` repeats the first.
interface AskPending extends PendingBase {
  kind: "ask";
  questions?: AskQuestion[];
}
interface ApprovalPending extends PendingBase {
  kind: "approval";
  approval?: {
    // The call in the words its row uses while it runs: "Running make test".
    action: string;
    preview?: string;
    // What approving risks, most serious first; then the reasons the
    // automatic review gave, by name and probability.
    risks?: { id: string; label: string }[];
    // The stage of the order that asks ("mode", "ask_rule", "review",
    // "refusals", "mandatory"), or "report" for an agent's report held back,
    // and why, in a line; an asking rule in words.
    source?: string;
    reason?: string;
    rule?: string;
  };
}
// A line of text, a password, a file to edit, or a step done in the browser.
interface InputPending extends PendingBase {
  kind:
    | "text"
    | "secret"
    | "editor"
    | "browser"
    | "project.trust"
    | "model.select";
}
// What an MCP server asks of the person: a flat form, or a page to open.
interface ElicitPending extends PendingBase {
  kind: "elicit";
  elicitation: Elicitation;
}
export type Pending =
  | AskPending
  | ApprovalPending
  | ElicitPending
  | InputPending;
// As the host sends it: a choice field's options are normalized in `choices`.
export interface ElicitField {
  type: "string" | "number" | "integer" | "boolean" | "array";
  title?: string;
  description?: string;
  format?: "email" | "uri" | "date" | "date-time";
  minLength?: number;
  maxLength?: number;
  minimum?: number;
  maximum?: number;
  choices: { value: string; label: string }[];
  default?: string | number | boolean | string[];
}
// Its reply text is the ElicitResult: {action, content?}.
export interface Elicitation {
  server: string;
  mode: "form" | "url";
  message: string;
  requestedSchema?: {
    properties: Record<string, ElicitField>;
    required?: string[];
  };
  url?: string;
  host?: string;
  punycode?: boolean;
}
// Each question also takes a free-text "Other" answer.
export interface AskQuestion {
  question: string;
  // A short chip label.
  header: string;
  options: AskOption[];
  multi_select?: boolean;
}
// What the agent showed of an option: an image it made, snapshotted into the
// session (id; the path is for terminals), and a monospace preview. The
// description is the image's alt text.
interface AskOption {
  label: string;
  description: string;
  image?: { id?: string; name?: string; path: string };
  preview?: string;
}
// An ask's reply text is the JSON array of these, one per question in order.
// An attachment_id is also listed in the reply's attachment_ids.
export interface AskAnswer {
  choices: string[];
  other: string;
  attachment_id?: string;
}
interface Permissions {
  mode: string;
  default: string;
  effective?: string;
  source?: string;
}
// A rule allows the calls of `tool` that `match` covers: "*" every call,
// "PREFIX *" a command, "DIR/" a path under it, else that exact subject.
export type RuleEffect = "allow" | "ask" | "deny";
interface PermissionRule {
  tool: string;
  match: string;
  effect: RuleEffect;
  created: number;
}
export interface PermissionRules {
  root: string;
  rules: PermissionRule[];
}
// Lifecycle status owned by the native session host.
type SessionStatus =
  | "draft"
  | "saved"
  | "starting"
  | "idle"
  | "running"
  | "waiting"
  | "processing"
  | "closing"
  | "interrupted"
  | "failed"
  | "updating"
  | "deleting";
// Where a conversation in Scratch belongs, as its host suggests: a folder,
// a conversation there to continue, or a workspace to create by name.
export interface PlaceOption {
  cwd?: string;
  session?: string;
  workspace?: string;
}
export interface Session {
  host?: string;
  file_origin?: string;
  suggestion?: {
    options?: PlaceOption[];
    reason?: string;
    dismissed?: boolean;
  } | null;
  task_id?: string;
  id: string;
  generation?: string;
  title?: string;
  cwd?: string;
  // The conversation this one belongs to: it started it as its agent, or
  // this is its side chat.
  parent?: string;
  agent_edits?: "none" | "worktree" | "folder";
  // An agent's name, and the call that started it (agents started together
  // share it).
  agent_name?: string;
  agent_call?: string;
  // Where a side chat branched from, also once it was opened as a
  // conversation of its own (detached). `request` is its first question's
  // request id; `asked` when, in ms; `merged` when its summary last reached
  // the conversation, and how many turns it had then.
  side?: {
    parent: string;
    request: string;
    asked: number;
    merged?: { at: number; turns: number };
    detached?: boolean;
  } | null;
  // A side chat summing itself up for its conversation.
  merging?: boolean;
  // The model its file names.
  model?: string;
  status?: SessionStatus;
  presence?: "active" | "";
  updated?: number;
  incoming?: number;
  turn_active?: boolean;
  needs_you?: boolean;
  // What it waits on you for: "approval", "ask", …, and its prompt.
  pending_prompt?: string;
  turn_status?: string;
  activities?: Activity[];
  guidance?: number;
  error?: string;
  // Its spend, its turns, and how far its to-do list is (the item in
  // progress).
  cost?: number;
  turns?: number;
  tokens?: number;
  // Its tool calls, finished turns' and the running turn's, live; when the
  // running turn started, 0 before a turn.
  tool_calls?: number;
  turn_started_ms?: number;
  todo?: { done: number; total: number; current: string } | null;
}
// A folder at a glance, from the host: what its conversations spent today
// and in 7 days, the files they changed today, and its repository.
export interface FolderFacts {
  spend: { today: number; week: number };
  changed: { files: number; added: number; removed: number };
  git?: { branch: string; uncommitted: number };
}
export type SessionRef = Pick<Session, "id" | "generation" | "file_origin">;
export interface State {
  phase?: ExecutionPhase;
  // When the running turn started, on the host's clock.
  turn_started_ms?: number;
  attachments?: number;
  title?: string;
  route?: string;
  // Where the model comes from: "conversation", "user", "project", …
  route_source?: string;
  effort?: string;
  efforts?: string[];
  view?: View;
  turns?: number;
  usage?: Usage;
  statistics?: Statistics;
  turn_status?: string;
  activities?: Activity[];
  todo?: TodoItem[];
  context_tokens?: number;
  context_window?: number;
  // Absent in a conversation saved before parts were recorded.
  context_parts?: ContextParts;
  permissions?: Permissions;
  http?: Exchange[];
  mcp?: McpServer[];
  error?: string;
  stop?: TurnStop | null;
}
export interface McpServer {
  name: string;
  scope: "global" | "project";
  file: string;
  state: "ready" | "starting" | "disabled" | "failed";
  command: string;
  overrides: boolean;
  tools: number;
  // What the server serves to this conversation, named `<server>:<path>`.
  skills: { name: string; description: string }[];
  error?: string;
  log?: string;
}
export type ExecutionPhase =
  | "idle"
  | "working"
  | "waiting"
  | "preparing"
  | "retrying"
  | "thinking"
  | "responding"
  | "tool"
  | "finishing"
  | "decision"
  // The turn waits for the agents it started; turn_status names them.
  | "agents";
export interface Snapshot {
  epoch?: string;
  cursor: number;
  metadata: Session;
  state?: State;
  pending?: Pending | null;
  live_truncated?: boolean;
}
export interface SlashCommand {
  command: string;
  argument: string;
  aliases: string[];
  usage: string;
  description: string;
  // Listed only at a terminal: this page has its own control for it. Typed
  // here it still runs, and its aliases still resolve.
  terminal?: boolean;
  // Navigation a client performs itself; a runtime refuses it.
  client_only?: boolean;
}
// A workspace: a folder the host keeps for conversations that belong to no
// folder, under the name its user gave it.
export interface Space {
  path: string;
  name: string;
}

// A conversation (by id), a folder (by path) or a folder's terminal (by id)
// kept at the top of the list.
export interface Pin {
  host?: string;
  kind: "session" | "folder" | "terminal";
  id: string;
}

export interface Catalogue {
  spaces?: Space[];
  pins?: Pin[];
  commands?: SlashCommand[];
  // The detail level, and whether thinking shows, until a device chooses
  // its own.
  detail?: string;
  thinking?: boolean;
  scheduled?: ScheduledState;
  // The folders' shells; null while paired devices may open none
  // (web.terminal).
  terminals?: TerminalInfo[] | null;
  epoch?: string;
  cursor?: number;
  sessions: Session[];
  devices: { id: string; name: string }[];
  device?: string;
  capabilities: {
    subscribed?: boolean;
    push?: boolean;
    push_reason?: string;
    vapid_public_key?: string;
  };
}
export interface Outcome {
  request_id: string;
  accepted?: boolean;
  pending?: boolean;
  unknown?: boolean;
  error?: string;
  code?: string;
}
interface EventData extends Omit<Partial<Exchange>, "status"> {
  request_id?: string;
  // approval.requested: "parent" while an agent's parent decides.
  route?: string;
  inspect?: boolean;
  context_tokens?: number;
  output?: string;
  usage?: Usage;
  statistics?: Statistics;
  node?: Node;
  text?: string;
  reset?: boolean;
  id?: string;
  turn?: number;
  request?: string;
  attempt?: number;
  name?: string;
  view?: ToolView;
  result?: JSONValue;
  activity_id?: number;
  parts?: ToolPart[];
  status?: string | number;
  duration_ms?: number;
  presentation?: {
    status?: string;
    title?: string;
  };
  error?: string;
  activities?: Activity[];
  permissions?: Permissions;
}
// A change to one row of the host's view: the whole row, or fields to set
// and streamed text to append on a row the client already holds.
export interface NodePatch {
  kind: "node";
  node?: Node;
  id?: string;
  set?: Pick<Node, "streaming">;
  append?: { text?: string };
}
// Host envelopes and native EventEmitter payloads share the same SSE channel.
interface HostEnvelope extends Partial<Omit<Outcome, "pending">> {
  scheduled?: ScheduledState;
  v: number;
  epoch: string;
  sequence: number;
  session_id: string;
  generation?: string;
  type?: string;
  // Set by the host on events a person should be told about (push).
  attention_id?: string;
  data?: EventData;
  metadata?: Session;
  state?: State;
  phase?: ExecutionPhase;
  turn_status?: string;
}
export type HostEvent = HostEnvelope &
  (
    | ({ kind: "outcome" } & Outcome)
    | { kind: "state"; pending?: Pending | null }
    | NodePatch
    | {
        kind:
          | "event"
          | "turn_status"
          | "metadata"
          | "activated"
          | "deactivated"
          | "error"
          | "gap"
          | "moved"
          | "management.changed"
          | "scheduled.changed";
      }
    | { kind: "deleted" }
    | { kind: "settings.changed"; level: string; thinking?: boolean }
    | { kind: "pins.changed"; pins: Pin[] }
    | { kind: "spaces.changed"; spaces: Space[] }
    | { kind: "terminals.changed"; terminals: TerminalInfo[] | null }
  );
export interface BodyPage {
  text: string;
  more: boolean;
  next: number;
  exchange?: Exchange;
}
interface Model {
  value: string;
  label: string;
  name?: string;
  active?: boolean;
  efforts?: string[];
}
interface ModelCatalogue {
  models: Model[];
  // Providers whose list did not come back: no key, or no answer.
  unavailable?: string[];
}
// One setting as the host states it: its description, then the facts.
// Where a setting's value can be saved, and every place it can come from.
export type ConfigScope = "user" | "project" | "conversation";
type ConfigSource = ConfigScope | "environment" | "cli";
export interface ConfigSetting {
  name: string;
  // Its name in the settings file: "limits.maxSteps".
  key: string;
  label: string;
  purpose?: string;
  description: string;
  // Where Settings lists it: a topic id and a group id of `groups`.
  topic: string;
  group: string;
  // Listed under Show all.
  advanced?: boolean;
  type: string;
  sensitivity: string;
  default?: JSONValue;
  minimum?: number;
  maximum?: number;
  choices?: string[];
  // Read by a terminal or by the host as it starts, not by a conversation.
  terminal?: boolean;
  // The scopes it may be saved at, lowest first.
  scopes: ConfigScope[];
  // What each scope holds (`true` for a secret); a scope that holds nothing
  // is absent, and so is `set` when none does.
  set?: Partial<Record<ConfigSource, JSONValue>>;
  // What applies now; absent for a secret.
  effective?: JSONValue;
  // The scope the value in effect comes from.
  source: "default" | ConfigSource;
  locked: boolean;
  // While empty: the setting it takes its value from, or a phrase.
  follows?: string;
  fallback?: string;
}
export interface ConfigGroup {
  id: string;
  label: string;
  topic: string;
}
export interface ConfigChange {
  key: string;
  value?: string;
  unset?: boolean;
}
export interface Configuration {
  settings: ConfigSetting[];
  // The file it is all saved in, and what in it the host could not take.
  file: string;
  problem: string;
  // That file as it stands, every secret hidden.
  document?: JSONValue;
  // The groups settings are listed under, in order, each in a topic.
  groups: ConfigGroup[];
  effects: {
    key: string;
    effect: "next_turn" | "restart" | "shadowed";
    text: string;
  }[];
}
export interface ToolCatalogueItem {
  name: string;
  title: string;
  description: string;
  category: string;
  provider: string;
  active: boolean;
  available: boolean;
  schema_bytes: number;
  reason?: string;
}
// What fills a conversation's context (Agent::ContextParts): estimated
// bytes per part, which sum to `bytes` and come to `tokens`; the tools
// counted by kind; compacts_at is 0 where nothing compacts.
export interface ContextParts {
  tokens: number;
  bytes: number;
  compacts_at: number;
  instructions: string;
  parts: Record<
    | "tool_results"
    | "replies"
    | "messages"
    | "files"
    | "notes"
    | "tools"
    | "mcp"
    | "system"
    | "instructions"
    | "memory",
    number
  >;
  tools: Partial<
    Record<"tools" | "mcp" | "hidden", { count: number; bytes: number }>
  >;
}
export interface ToolCatalogue {
  profile: string;
  base_profile: string;
  profiles: string[];
  active: number;
  available: number;
  schema_bytes: number;
  full_schema_bytes: number;
  tools: ToolCatalogueItem[];
}
// One instruction file a person edits: for every conversation or a folder's
// panel, yours or the folder's.
export interface InstructionFile {
  scope: "user" | "project";
  path: string;
  text: string;
}
export interface InstructionStack {
  files: InstructionFile[];
  also_loaded: string[];
  // The system message: its sections' built-in text, and the file of a
  // person's changes as it is saved ("" when there is none).
  prompt: { sections: { name: string; text: string }[]; saved: string };
}
export interface CommandResults {
  browser: {
    running?: boolean;
    mode?: string;
    created_profile_id?: string;
  };
  instructions: InstructionStack;
  memory: LibraryResult;
  skills: LibraryResult;
  import: ImportResult;
  schedule: ScheduleResult;
  models: ModelCatalogue;
  model: ModelCatalogue;
  provider: { configured: boolean };
  openrouter: { url: string; model?: string };
  config: Configuration;
  restart_conversations: { restarting: number; deferred: number };
  space: { spaces: Space[] };
  pins: { pins: Pin[] };
  restart_host: { restarting: boolean };
  update: UpdateState;
  tools: ToolCatalogue;
  activity: ActivityDetail;
  context: { exchanges: Exchange[] };
  revert: { restored: string[]; conflicts: { path: string; reason: string }[] };
  // A turn's change to a file, as a unified diff.
  changes: { path: string; text: string };
  // A fork cut before a message returns that message, to edit (prompt).
  fork: { id: string; prompt?: string };
  share: { path: string };
  side: { id: string };
  // The conversation's new id: moving changes it.
  move: { id: string };
  create: never;
  activate: never;
  close: never;
  rename: never;
  keep: never;
  delete: never;
  logout: never;
  submit: never;
  steer: never;
  recall: never;
  insert: never;
  reply: never;
  interrupt: never;
  permissions: Permissions & {
    sandbox?: { summary?: string; network?: string };
  };
  permission_rules: PermissionRules;
  revoke_device: never;
  test_notification: never;
  terminal: TerminalList;
}
export type CommandKind = keyof CommandResults;
type None = Record<never, never>;
interface Library {
  cwd?: string;
  action: string;
  key?: string;
  revision?: string;
  content?: string;
  target?: string;
}
interface Message {
  text: string;
  attachment_ids?: { id: string; name: string }[];
  // Steer: hold the message until the running turn ends ("Queue next").
  queue?: boolean;
}
// What each command sends besides its kind and conversation.
export interface CommandInputs {
  browser: {
    action: string;
    instance_id?: string;
    interaction_id?: string;
    name?: string;
    profile_id?: string;
    parallel_capacity?: number;
    viewport?: string;
  };
  instructions: {
    action: string;
    cwd?: string;
    scope?: string;
    text?: string;
    // The text an edit started from; a changed file refuses.
    base?: string;
  };
  memory: Library;
  skills: Library;
  // only: limits `list` and `run` to skills or memories.
  import: { cwd?: string; action: string; only?: string; key?: string };
  schedule: {
    action: string;
    key?: string;
    revision?: string;
    task?: ScheduledTask;
    schedule?: ScheduleRule;
  };
  models: { cwd?: string };
  provider: object;
  openrouter: {
    action: "start" | "finish";
    callback?: string;
    code?: string;
    state?: string;
  };
  model: {
    operation: string;
    model?: string;
    effort?: string;
    // Select: also save the choice for all conversations.
    default?: boolean;
  };
  config: {
    operation?: string;
    scope?: string;
    cwd?: string;
    changes?: ConfigChange[];
  };
  restart_conversations: { target_id?: string; cwd?: string };
  space: { action: string; cwd: string; name: string };
  pins: { pins: Pin[] };
  restart_host: None;
  update: {
    action: "show" | "check" | "apply" | "chrome";
    version?: string;
  };
  tools: {
    operation: string;
    name?: string;
    active?: boolean;
    profile?: string;
    enabled?: boolean;
  };
  activity: {
    operation: string;
    activity_id?: number;
  };
  context: None;
  // The turn (0 = latest) and one file of it, or all.
  revert: { turn: number; path?: string };
  changes: { turn: number; path: string };
  // argument: raw text after /fork, parsed by the native host; message_id:
  // the message a fork is cut before ("m-<id>").
  fork: { argument?: string; message_id?: string };
  share: None;
  // Starts a side chat of the conversation, whose first question goes as
  // `request`; merges this one back, or opens it as a conversation of its own.
  side:
    | { operation: "create"; text: string; request: string }
    | { operation: "merge" | "detach" };
  // The folder or workspace to move to, by its path on the host.
  move: { cwd?: string; space?: string };
  // Answers a suggested place with "here".
  keep: None;
  // A workspace by its name (`space`), in place of `cwd`.
  create: { cwd?: string; space?: string };
  activate: None;
  close: None;
  rename: { title: string };
  delete: None;
  logout: None;
  submit: Message;
  steer: Message;
  recall: { target_id: string };
  insert: { target_id: string };
  reply: {
    interaction_id: string;
    text: string;
    cancelled?: boolean;
    attachment_ids?: string[];
  };
  interrupt: None;
  // A mode, or a call a review refused allowed for this conversation.
  permissions: {
    mode?: string;
    inspect?: boolean;
    allow?: { tool: string; match: string };
  };
  permission_rules: {
    action: string;
    tool?: string;
    match?: string;
    effect?: RuleEffect;
    cwd?: string;
  };
  revoke_device: { device_id: string };
  test_notification: None;
  // A shell in a folder, the home folder without one; or the end of one, or
  // a new name for it.
  terminal:
    | { action: "open"; cwd?: string; folder?: string }
    | { action: "rename"; id: string; title: string }
    | { action: "close"; id: string };
}
export type CommandFields<K extends CommandKind> = CommandInputs[K] & {
  request_id?: string;
};
type Receipt<T> = Outcome &
  ({ pending: true } | { pending?: false; result: T });
export type CommandReceipt<K extends CommandKind> = Receipt<CommandResults[K]> &
  (K extends "create" ? { session: Session } : object);
export type Act = <K extends CommandKind>(
  kind: K,
  fields?: CommandFields<K>,
) => Promise<CommandReceipt<K>>;
export type StatisticsModal =
  | {
      type: "rename" | "delete";
      session: Session;
      space?: never;
      terminal?: never;
      node?: never;
      snapshot?: never;
    }
  // A workspace, by its name; deleting says how many conversations go with it.
  | {
      type: "rename" | "delete";
      space: Space;
      conversations?: number;
      session?: never;
      terminal?: never;
      node?: never;
      snapshot?: never;
    }
  // A folder's terminal, which can be renamed.
  | {
      type: "rename";
      terminal: TerminalInfo;
      session?: never;
      space?: never;
      node?: never;
      snapshot?: never;
    }
  // One turn's or message's; a conversation's are in Settings.
  | {
      type: "statistics";
      session_id: string;
      node_id: string;
      // What node_id names: a turn (its footer) or one message.
      unit: StatisticsUnit;
    };
export type StatisticsUnit = "Turn" | "Message";
export interface RawOptions {
  part?: "request" | "response";
  session?: string;
  id?: string;
  value?: JSONValue;
  exchanges?: Exchange[];
  context?: boolean;
  prepare?: SessionRef;
}
export type AppModal =
  // handoff: opened for the agent's request (take control, close on Done);
  // session: whose browser, when not the open conversation's (a side chat).
  | { type: "browser"; handoff?: boolean; session?: string }
  | { type: "instructions" }
  | StatisticsModal
  | ({ type: "raw" } & RawOptions)
  | { type: "new"; place: "conversation" | "workspace" | "folder" }
  // `sessions`: several moved at once, from the sidebar's selection.
  | { type: "move"; session: Session; sessions?: Session[] }
  | {
      type: "file-origin";
      path: string;
      choices: FileOrigin[];
      pick: (origin: string) => void;
      linked: boolean;
    }
  | { type: "delete-many"; sessions: Session[]; spaces: Space[] }
  // address: where Settings opens, "approvals/rules"; linked: opened from
  // the page's own address, which Back leaves.
  | { type: "settings"; address?: string; linked?: boolean };

export interface LibraryItem {
  provenance?: {
    automatic: boolean;
    source_session: string;
    timestamp: string;
  };
  key: string;
  name: string;
  path: string;
  scope: string;
  source: string;
  writable: boolean;
  revision: string;
  content?: string;
  bytes?: number;
  modified?: number;
  description?: string;
  status?: string;
  error?: string;
  required_tools?: string[];
  files?: string[];
  // In the Library's listing: what it is and the place it belongs to, ""
  // for every place's; an artifact that is one file, not a folder, is alone.
  kind?: LibraryKind;
  place?: string;
  alone?: boolean;
  // A design's page count and first pages; a shared file's conversation
  // (its key is the asset's id).
  count?: number;
  pages?: string[];
  session?: string;
}
export type LibraryKind = "memory" | "skill" | "file" | ArtifactType;
// A folder or workspace the Library lists, with its latest conversation.
export interface LibraryPlace {
  // Local places use their path as the key; SSH workspaces also name the host.
  key?: string;
  host?: string;
  path: string;
  name: string;
  workspace: boolean;
  session?: string;
}
export interface LibraryListing {
  places: LibraryPlace[];
  items: LibraryItem[];
  limits: { memory: number; skill: number; memory_enabled: boolean };
}
export interface ImportItem {
  key: string;
  kind: "skill" | "memory";
  source: "claude" | "codex";
  name: string;
  path: string;
  bytes: number;
  collides: boolean;
  // Why it cannot be imported as it is.
  blocked?: string;
  needs_model?: boolean;
}
interface ImportResult {
  items?: ImportItem[];
  skipped?: { kind: string; name: string; reason: string }[];
}
interface LibraryResult {
  enabled?: boolean;
  items?: LibraryItem[];
  item?: LibraryItem;
  limit?: number;
  applies?: string;
  deleted?: string;
}
export interface ScheduleRule {
  type: "once" | "interval" | "weekly";
  at?: number;
  start?: number;
  seconds?: number;
  time?: string;
  days?: number[];
  timezone?: string;
}
export interface ScheduledTask {
  host?: string;
  id: string;
  revision: string;
  name: string;
  prompt: string;
  // A shell command run before each run: exit 0 starts it, 1 skips it.
  check: string;
  cwd: string;
  model: string;
  permissions: "ask" | "auto" | "yolo" | "plan";
  environment: "local" | "worktree";
  schedule: ScheduleRule;
  enabled: boolean;
  next?: number;
  upcoming?: number[];
}
interface ScheduledRun {
  session_available?: boolean;
  id: string;
  task_id: string;
  title: string;
  scheduled_for: number;
  updated: number;
  status: string;
  cwd: string;
  project: string;
  session_id: string;
  error?: string;
}
export interface ScheduledState {
  error?: string;
  tasks: ScheduledTask[];
  runs: ScheduledRun[];
  revision: string;
}
interface ScheduleResult extends Partial<ScheduledState> {
  item?: ScheduledTask;
  run?: ScheduledRun;
  times?: number[];
  error?: string;
}

export type ConnectionPhase =
  | "connected"
  | "connecting"
  | "reconnecting"
  | "disconnected";

// What the inspector is opened on: a transcript row, a live activity, or a
// tool's retained input/output.
export type InspectorTarget =
  | { node: ActivityTarget }
  | { item: Activity }
  | { raw: RawOptions; title: string };

// A shell the web host runs for paired devices: where it started, and the
// folder or workspace it belongs to ("" for the page's own, which ends with
// its view). A folder's shell has a name and a start time; the page's own
// has neither.
export interface TerminalInfo {
  host?: string;
  id: string;
  cwd: string;
  folder: string;
  title: string;
  started: number;
}
// The folders' shells, and the one just opened.
interface TerminalList {
  id?: string;
  terminals: TerminalInfo[];
}
// A terminal socket's text frames; its binary frames are the terminal's
// bytes. In: the size the page shows it at. Out: the shell ended (code -1
// when unknown), or a newer socket took it over.
export type TerminalControl = { type: "resize"; rows: number; cols: number };
export type TerminalNotice =
  | { type: "exit"; code: number }
  | { type: "replaced" };

// This build, the newest release last read from GitHub, and what updating
// this install takes: `apply` when the host can run it, else `command`.
export interface UpdateState {
  version: string;
  install: string;
  latest?: string;
  notes?: string;
  url?: string;
  checked_ms?: number;
  available: boolean;
  apply: boolean;
  blocked: string;
  command: string;
  installing?: boolean;
  output?: string;
  error?: string;
  // A container's own Chrome: "ready VERSION", "downloading VERSION" or
  // "failed REASON", and when it last looked.
  chrome?: { status: string; checked_ms: number; checking?: boolean };
}
