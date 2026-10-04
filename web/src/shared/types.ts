// The native host owns this versioned wire contract. Unknown JSON stays at the
// HTTP boundary; components share these projections instead of redefining them.
export type JSONValue =
  null | boolean | number | string | JSONValue[] | { [key: string]: JSONValue };
// "inline": the surface that failed shows it; the banner stays clear.
export type Report = (error: unknown, scope?: "inline") => void;
export interface Failure extends Error {
  network?: boolean;
  status?: number;
  rejected?: boolean;
}
export function failure(error: unknown): Failure {
  return error instanceof Error ? error : new Error(String(error));
}
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
  turn_root?: string;
}
export interface ToolActivity {
  // The call's intent: explore, research, edit, verify, run, setup,
  // delegate, memory or share.
  category?: string;
  label?: string;
  group?: { id: string; label: string };
}
// How a call reads, built natively by each tool (see ToolView in tool.h).
export type ToolPart =
  | { kind: "command"; text: string }
  | { kind: "code"; text: string; language?: string; label?: string }
  | { kind: "fields"; rows: [string, string][] }
  | FilePart
  | LinkPart;
export interface FilePart {
  kind: "file";
  id: string;
  name: string;
  mime: string;
  bytes: number;
}
export interface LinkPart {
  kind: "link";
  to: "agent" | "activity" | "memory";
  id: string | number;
  label: string;
}
// "Editing a.ts" while running, "Edited a.ts" after; input shows on expand
// and a "tail" result keeps its last lines visible.
export interface ToolView {
  verb?: [string, string];
  target?: string;
  input?: ToolPart[];
  output?: "text" | "markdown" | "tail";
}
export interface TurnSummary {
  usage_reported?: boolean;
  turn: number;
  outcome: string;
  route?: string;
  tool_calls: number;
  direct_tool_calls?: number;
  model_calls?: number;
  direct_model_calls?: number;
  background_statistics?: Statistics;
  steps: number;
  duration_ms: number;
  ttt_ms: number;
  tokens_per_second: number;
  usage: Usage;
  // Files the turn's edit, write and delete tools changed (at most 50).
  files?: TurnFile[];
}
export interface TurnFile {
  path: string;
  added: number;
  removed: number;
  undoable: boolean;
}
// Why the last turn ended; null while one runs.
export interface TurnStop {
  reason: string;
  detail?: string;
}
export interface Block {
  memory?: {
    action: string;
    key: string;
    automatic: boolean;
    minor?: boolean;
  };
  activity?: ToolActivity;
  sequence?: number;
  summary?: TurnSummary;
  compaction?: {
    automatic: boolean;
    messages_before: number;
    messages_after: number;
    duration_ms: number;
  };
  deliveries?: { name: string; delivery: string }[];
  id: string;
  response_id?: string;
  occurrence_id?: string;
  content_revision?: number;
  content_complete?: boolean;
  text_bytes?: number;
  retained_text_bytes?: number;
  kind: string;
  text?: string;
  time?: string;
  reasoning?: string;
  streaming?: boolean;
  request_id?: string;
  session_id?: string;
  incoming?: number;
  files?: Asset[];
  origin?: string;
  // Tool calls whose results added these files to context.
  source_call_ids?: string[];
  unavailable_images?: number;
  call_id?: string;
  name?: string;
  arguments?: JSONValue;
  view?: ToolView;
  detail_id?: string;
  status?: string;
  error?: string;
  duration_ms?: number;
  // Retained tool_result whose receipt facts are gone: its fallbacks
  // ("tool"/"running") must never shadow the call record it joins.
  ttft_ms?: number;
  tokens_per_second?: number;
  route?: string;
  change?: string;
  change_path?: string; // set when `change` is only a stored diff's opening
  truncated?: boolean;
  usage?: Usage;
  usage_reported?: boolean;
  http?: Exchange[];
  turn_root?: string;
  activity_id?: number;
  agent_id?: string;
  // Parts that stay visible on the row: files shared, work started.
  parts?: ToolPart[];
  // The end of a result whose text preview holds only its start.
  tail?: string;
  command?: string;
}
export interface PresentedBlock extends Block {
  children?: PresentedBlock[];
  // A folded row's own line: the host's group label, or a turn's work.
  label?: string;
  key?: string;
  source?: Block;
  result_loaded?: boolean;
}
export interface Outgoing extends Block {
  request_id: string;
  session_id: string;
}
export interface View {
  blocks: Block[];
  before?: number;
  more?: boolean;
  dropped_segments?: number;
  fork?: { title: string; turns: number };
}
export interface Activity {
  id?: number;
  activity_id?: number;
  agent_id?: string;
  kind?: string;
  label?: string;
  name?: string;
  description?: string;
  command?: string;
  status?: string;
  started_ms?: number;
  duration_ms?: number;
  progress?: string;
  model?: string;
  // A command the turn is still waiting on, until moved to the background.
  detached?: boolean;
}
// A child session this conversation delegated to, running or resumable.
export interface Agent {
  id: string;
  label?: string;
  name?: string;
  description?: string;
  model?: string;
  status?: string;
}
export interface ActivityDetail extends Activity {
  activity_detail?: ActivityStatusDetail | null;
  phase?: ExecutionPhase;
  context_tokens?: number;
  context_window?: number;
  route?: string;
  olderWindow?: boolean;
  body?: BodyPage;
  command?: string;
  memory?: Block["memory"];
  task?: string;
  directive?: string;
  system_prompt?: string;
  conversation?: View;
  output?: string;
  statistics?: Statistics;
  usage?: Usage;
  turns?: number;
}
export interface Pending {
  id: string;
  kind?: string;
  initial?: string;
  prompt?: string;
  options?: (string | { value: string; label?: string; title?: string })[];
  approval?: {
    tool: string;
    mandatory_human?: boolean;
    mandatory_reason?: string;
    preview?: string;
    // What approving risks, most serious first.
    risks?: { id: "runs" | "writes" | "network" | "outside"; label: string }[];
  };
  // kind "ask": the model's questions; `prompt` repeats the first.
  questions?: AskQuestion[];
  // A thread's decision goes to its coordinator first; "human" once yielded.
  route?: "coordinator" | "human";
  note?: string;
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
export interface AskOption {
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
export interface Permissions {
  mode: string;
  default: string;
  effective?: string;
}
export interface PermissionRule {
  key: string;
  tool: string;
  preview: string;
  created: number;
}
export interface PermissionRules {
  root: string;
  rules: PermissionRule[];
}
// Lifecycle status owned by the native session host.
export type SessionStatus =
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
export interface Session {
  task_id?: string;
  id: string;
  generation?: string;
  title?: string;
  cwd?: string;
  // A folder's coordinator, or a thread it launched (in `folder`).
  kind?: "coordinator" | "thread" | "";
  folder?: string;
  status?: SessionStatus;
  presence?: "active" | "";
  updated?: number;
  incoming?: number;
  turn_active?: boolean;
  pending?: boolean;
  // What it waits on you for: "approval", "ask", …, and its prompt.
  pending_prompt?: string;
  activity?: string;
  activities?: Activity[];
  guidance?: number;
  error?: string;
}
export type SessionRef = Pick<Session, "id" | "generation">;
export interface ActivityStatusDetail {
  source: "lifecycle" | "tool" | "model_intent" | "summary" | "reasoning";
  label: string;
  response_id: string;
  occurrence_id: string;
  active_tools: number;
  turn: number;
  retry?: { attempt: number; max_attempts: number; retry_at_ms: number };
}
export interface State {
  phase?: ExecutionPhase;
  activity_detail?: ActivityStatusDetail | null;
  attachments?: number;
  title?: string;
  route?: string;
  effort?: string;
  efforts?: string[];
  variant?: string;
  variants?: string[];
  view?: View;
  // Bumped by an in-place rewind, which replaces the view.
  view_epoch?: number;
  turns?: number;
  usage?: Usage;
  system_prompt?: string;
  // The agent's own conversation-scoped addition (adapt_system).
  self_directive?: SelfDirective;
  // Why a coordinator holds its threads' events (today's spend limit).
  paused?: string;
  statistics?: Statistics;
  activity?: string;
  activities?: Activity[];
  agents?: Agent[];
  context_tokens?: number;
  context_window?: number;
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
  | "searching"
  | "finishing"
  | "decision";
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
}
// What one verbosity level shows, as the host's table states it (see
// DetailPolicy in verbosity.h): how tool work is laid out, and what is shown
// without asking.
export interface DetailPolicy {
  work: "turn" | "groups" | "calls";
  // Thinking: absent, a closed row, or shown.
  reasoning: "hidden" | "closed" | "open";
  open: boolean;
  minor: boolean;
}
export interface Verbosity {
  level: string;
  levels: Record<string, DetailPolicy>;
}
export interface Catalogue {
  commands?: SlashCommand[];
  verbosity?: Verbosity;
  scheduled?: ScheduledState;
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
}
export interface EventData extends Omit<Partial<Exchange>, "status"> {
  request_id?: string;
  // approval.requested: "coordinator" while a thread's coordinator decides.
  route?: string;
  inspect?: boolean;
  context_tokens?: number;
  output?: string;
  usage?: Usage;
  statistics?: Statistics;
  block?: Block;
  text?: string;
  reset?: boolean;
  id?: string;
  response_id?: string;
  occurrence_id?: string;
  call_id?: string;
  detail_id?: string;
  turn?: number;
  request?: string;
  attempt?: number;
  name?: string;
  arguments?: JSONValue;
  view?: ToolView;
  result?: JSONValue;
  agent_id?: string;
  activity_id?: number;
  parts?: Block["parts"];
  status?: string | number;
  duration_ms?: number;
  activity?: ToolActivity;
  presentation?: {
    change?: string;
    status?: string;
    title?: string;
    summary?: string;
    activity?: ToolActivity;
  };
  error?: string;
  activities?: Activity[];
  permissions?: Permissions;
}
// A change to one row of the host's view: the whole row, or fields to set
// and streamed text to append on a row the client already holds.
export interface BlockPatch {
  kind: "block";
  block?: Block;
  id?: string;
  set?: Partial<Block>;
  append?: { text?: string; reasoning?: string };
}
// Host envelopes and native EventEmitter payloads share the same SSE channel.
interface HostEnvelope extends Partial<Omit<Outcome, "pending">> {
  activity_detail?: ActivityStatusDetail | null;
  scheduled?: ScheduledState;
  v: number;
  epoch: string;
  sequence: number;
  session_id: string;
  generation?: string;
  time?: string;
  type?: string;
  // Set by the host on events a person should be told about (push).
  attention_id?: string;
  data?: EventData;
  metadata?: Session;
  state?: State;
  phase?: ExecutionPhase;
  guidance?: number;
  presence?: "active" | "";
  checkpoint?: boolean;
  updated?: number;
  activity?: string;
}
export type HostEvent = HostEnvelope &
  (
    | ({ kind: "outcome" } & Outcome)
    | { kind: "state"; pending?: Pending | null }
    | BlockPatch
    | {
        kind:
          | "event"
          | "activity"
          | "metadata"
          | "activated"
          | "deactivated"
          | "closed"
          | "deleted"
          | "error"
          | "gap"
          | "management.changed"
          | "scheduled.changed";
      }
    | { kind: "verbosity.changed"; level: string }
  );
export interface BodyPage {
  text: string;
  more: boolean;
  next: number;
  exchange?: Exchange;
}
export interface Model {
  value: string;
  label: string;
  name?: string;
  active?: boolean;
  efforts?: string[];
  variants?: string[];
}
export interface ModelCatalogue {
  models: Model[];
}
// One setting as the host states it: its description, then the facts.
// Where a setting's value can be saved, and every place it can come from.
export type ConfigScope = "user" | "project" | "conversation";
export type ConfigSource = ConfigScope | "file" | "environment" | "cli";
export interface ConfigSetting {
  name: string;
  label: string;
  purpose?: string;
  description: string;
  category: string;
  type: string;
  sensitivity: string;
  takes_effect: string;
  default?: JSONValue;
  minimum?: number;
  maximum?: number;
  choices?: string[];
  // Listed only in a terminal and the config file.
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
export interface ConfigChange {
  key: string;
  value?: string;
  unset?: boolean;
}
export interface Configuration {
  settings: ConfigSetting[];
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
export interface ToolCategory {
  id: string;
  name: string;
  created: number;
}
export interface ToolCategories {
  categories: ToolCategory[];
  assignments: Record<string, string>;
}
// One instruction file a person edits: for every session or a folder's
// coordinator, yours or the project's.
export interface InstructionFile {
  audience: "sessions" | "coordinator";
  scope: "user" | "project";
  path: string;
  text: string;
}
export interface InstructionStack {
  files: InstructionFile[];
  also_loaded: string[];
  base: { sessions: string; coordinator: string };
}
export interface SelfDirective {
  mode: "overlay" | "replace";
  text: string;
  revision: string;
}
export interface CommandResults {
  browser: {
    running?: boolean;
    mode?: string;
    created_profile_id?: string;
  };
  instructions: InstructionStack;
  self_directive: { item: SelfDirective };
  memory: LibraryResult;
  skills: LibraryResult;
  schedule: ScheduleResult;
  models: ModelCatalogue;
  model: ModelCatalogue;
  config: Configuration;
  restart_conversations: { restarting: number; deferred: number };
  restart_host: { restarting: boolean };
  tools: ToolCatalogue;
  tool_categories: ToolCategories;
  activity: ActivityDetail;
  context: { exchanges: Exchange[] };
  revert: { restored: string[]; conflicts: { path: string; reason: string }[] };
  // A fork cut before a message returns that message, to edit (prompt).
  // A coordinator rewinds itself in place (`rewound`) instead of forking.
  fork: { id: string; prompt?: string; rewound?: boolean };
  share: { path: string };
  side: { answer: string };
  create: never;
  activate: never;
  close: never;
  rename: never;
  delete: never;
  logout: never;
  submit: never;
  steer: never;
  recall: never;
  reply: never;
  interrupt: never;
  permissions: Permissions;
  permission_rules: PermissionRules;
  revoke_device: never;
  test_notification: never;
}
export type CommandKind = keyof CommandResults;
export interface CommandFields {
  // Raw text after a slash command, parsed by the native host.
  argument?: string;
  // The message a fork is cut before ("m-<id>").
  message_id?: string;
  detail?: string;
  raw?: boolean;
  offset?: number;
  cancelled?: boolean;
  action?: string;
  revision?: string;
  content?: string;
  target?: string;
  task?: ScheduledTask;
  schedule?: ScheduleRule;
  after?: number;
  request_id?: string;
  target_id?: string;
  operation?: string;
  model?: string;
  effort?: string;
  variant?: string;
  mode?: string;
  profile?: string;
  active?: boolean;
  name?: string;
  scope?: string;
  changes?: ConfigChange[];
  key?: string;
  value?: string;
  cwd?: string;
  coordinator?: boolean;
  audience?: string;
  // Instructions: the text an edit started from; a changed file refuses.
  base?: string;
  title?: string;
  device_id?: string;
  text?: string;
  interaction_id?: string;
  profile_id?: string;
  category_id?: string;
  attachment_ids?: (string | { id: string; name: string })[];
  activity_id?: number;
  agent_id?: string;
  before?: number;
  // Steer: hold the message until the running turn ends ("Queue next").
  queue?: boolean;
  // Revert: the turn (0 = latest) and one file of it, or all.
  turn?: number;
  path?: string;
}
export type Receipt<T> = Outcome &
  ({ pending: true } | { pending?: false; result: T });
export type CommandReceipt<K extends CommandKind> = Receipt<CommandResults[K]> &
  (K extends "create" ? { session: Session } : object);
export type Act = <K extends CommandKind>(
  kind: K,
  fields?: CommandFields,
) => Promise<CommandReceipt<K>>;
export type StatisticsModal =
  | {
      type: "rename" | "delete";
      session: Session;
      block?: never;
      snapshot?: never;
    }
  | {
      type: "statistics";
      session_id: string;
      block_id?: string;
      // What block_id names: a turn (its footer) or one message.
      unit?: StatisticsUnit;
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
  // handoff: opened for the agent's request (take control, close on Done).
  | { type: "browser"; handoff?: boolean }
  | { type: "instructions" }
  | StatisticsModal
  | ({ type: "raw" } & RawOptions)
  | { type: "new" }
  // section: a settings section to open on, e.g. from /permissions.
  | { type: "settings"; section?: string }
  | { type: "tools"; session_id: string };

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
}
export interface LibraryResult {
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
  id: string;
  revision: string;
  name: string;
  prompt: string;
  cwd: string;
  model: string;
  permissions: "prompt" | "auto" | "yolo";
  environment: "local" | "worktree";
  schedule: ScheduleRule;
  enabled: boolean;
  next?: number;
  upcoming?: number[];
}
export interface ScheduledRun {
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
export interface ScheduleResult extends Partial<ScheduledState> {
  item?: ScheduledTask;
  run?: ScheduledRun;
  times?: number[];
  error?: string;
}
