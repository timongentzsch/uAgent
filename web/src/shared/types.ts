// The native host owns this versioned wire contract. Unknown JSON stays at the
// HTTP boundary; components share these projections instead of redefining them.
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
  web_searches?: number;
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
  turn_root?: string;
  served_by?: string;
  generation_id?: string;
}
interface ToolActivity {
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
  direct_tool_calls: number;
  model_calls?: number;
  direct_model_calls: number;
  background_statistics?: Statistics;
  steps: number;
  duration_ms: number;
  ttt_ms: number;
  tokens_per_second: number;
  usage: Usage;
  // Files the turn's edit, write and delete tools changed (at most 50).
  files?: TurnFile[];
  turn_root?: string;
  generation_ms?: number;
  generated_tokens?: number;
  messages?: number;
  context_tokens?: number;
  line_open?: boolean;
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
export interface MemoryNote {
  action: string;
  key: string;
  automatic: boolean;
  minor?: boolean;
}
// The fields every block carries; `kind` says which others it has.
interface BlockBase {
  id: string;
  sequence?: number;
  time?: string;
  text?: string;
  truncated?: boolean;
  text_bytes?: number;
  content_complete?: boolean;
  status?: string;
  turn_root?: string;
}
// How an attachment reached the model.
interface Delivery {
  id: string;
  name: string;
  delivery: string;
  path: string;
}
// A message a turn answers; its failed model calls keep their HTTP capture
// on it, as the reply that never came cannot.
interface Answered {
  reply_to?: string;
  http?: Exchange[];
}
// What a person typed, or mail from another session or the harness.
export interface UserBlock extends BlockBase, Answered {
  kind: "user";
  request_id?: string;
  session_id?: string;
  files?: Asset[];
  origin?: string;
  deliveries?: Delivery[];
  unavailable_images?: number;
  // Why the host refused it, on a message held here until it confirms.
  error?: string;
}
// Files added to context: a person's, or those a tool's result brought.
export interface AttachmentBlock extends BlockBase, Answered {
  kind: "attachment";
  request_id?: string;
  files?: Asset[];
  origin?: string;
  // Tool calls whose results added these files to context.
  source_call_ids?: string[];
  deliveries?: Delivery[];
  unavailable_images?: number;
}
export interface AssistantBlock extends BlockBase, Answered {
  kind: "assistant";
  incoming?: number;
  response_id?: string;
  content_revision?: number;
  reasoning?: string;
  reasoning_revision?: number;
  reasoning_complete?: boolean;
  reasoning_bytes?: number;
  streaming?: boolean;
  request_id?: string;
  files?: Asset[];
  deliveries?: Delivery[];
  unavailable_images?: number;
  duration_ms?: number;
  ttft_ms?: number;
  tokens_per_second?: number;
  route?: string;
  served_by?: string;
  usage?: Usage;
  usage_reported?: boolean;
}
// One tool call and its result, joined into one row.
export interface ToolBlock extends BlockBase, Answered {
  kind: "tool_result";
  response_id?: string;
  occurrence_id?: string;
  call_id?: string;
  detail_id?: string;
  name?: string;
  arguments?: JSONValue;
  view?: ToolView;
  activity?: ToolActivity;
  duration_ms?: number;
  change?: string;
  change_path?: string; // set when `change` is only a stored diff's opening
  // Parts that stay visible on the row: files shared, work started.
  parts?: ToolPart[];
  // The end of a result whose text preview holds only its start.
  tail?: string;
  exchange_path?: string;
  // A retained result whose receipt is gone.
  artifact?: boolean;
  // Files a tool's attachment block brought; the page joins them to the row.
  files?: Asset[];
}
// A note the host adds: a finished background command or agent, or a memory
// saved, changed or forgotten.
export interface ActivityBlock extends BlockBase {
  kind: "activity";
  incoming?: number;
  activity_id?: number;
  agent_id?: string;
  command?: string;
  activity?: ToolActivity;
  view?: ToolView;
  parts?: ToolPart[];
  memory?: MemoryNote;
}
// The footer a turn leaves.
export interface TurnSummaryBlock extends BlockBase {
  kind: "turn_summary";
  summary: TurnSummary;
}
export interface CompactionBlock extends BlockBase {
  kind: "compaction";
  compaction: {
    automatic: boolean;
    messages_before: number;
    messages_after: number;
    duration_ms: number;
  };
}
export type Block =
  | UserBlock
  | AttachmentBlock
  | AssistantBlock
  | ToolBlock
  | ActivityBlock
  | TurnSummaryBlock
  | CompactionBlock;
// What the inspector opens: a block's own work, or the one a link names.
export type ActivityTarget = Pick<
  ActivityBlock,
  | "id"
  | "status"
  | "text"
  | "command"
  | "activity"
  | "activity_id"
  | "agent_id"
  | "memory"
>;
// Rows the page makes of blocks. A fold of several is a group; the rest are
// the host's blocks.
interface GroupBlock extends BlockBase {
  kind: "group";
}
interface Presented {
  children?: PresentedBlock[];
  // A folded row's own line: the host's group label, or a turn's work.
  label?: string;
  key?: string;
  // The reply that made a tool call, as much of it as the row shows.
  source?: Pick<
    AssistantBlock,
    "kind" | "id" | "response_id" | "time" | "http"
  >;
  result_loaded?: boolean;
}
export type PresentedBlock = (Block | GroupBlock) & Presented;
// A row that reads as a tool call: a call and its result, or a receipt.
export type ToolRowBlock = (ToolBlock | ActivityBlock) & Presented;
// A message of yours, held until the host confirms it.
export type Outgoing = UserBlock & { request_id: string; session_id: string };
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
  kind?: "subagent" | "command";
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
  // The panel plan step it works for.
  step?: string;
  // Every token its calls processed, as of its last save.
  tokens?: number;
}
// A panel's plan (its plan tool): what the lead agent keeps between turns.
export interface PlanStep {
  id: string;
  title: string;
  status: "open" | "running" | "done" | "failed";
  result?: string;
}
export interface Plan {
  goals?: string;
  decisions?: string;
  open_questions?: string;
  steps?: PlanStep[];
}
export interface ActivityDetail extends Activity {
  phase?: ExecutionPhase;
  context_tokens?: number;
  context_window?: number;
  route?: string;
  olderWindow?: boolean;
  body?: BodyPage;
  command?: string;
  memory?: MemoryNote;
  task?: string;
  directive?: string;
  system_prompt?: string;
  conversation?: View;
  output?: string;
  statistics?: Statistics;
  usage?: Usage;
  turns?: number;
}
interface PendingBase {
  id: string;
  prompt?: string;
  initial?: string;
  // Terminal text shown above the prompt; the web draws `options` instead.
  description?: string;
  options?: (string | { value: string; label?: string; title?: string })[];
  // A thread's decision goes to its panel first; "human" once yielded.
  route?: "panel" | "human";
  note?: string;
}
// The model's questions; `prompt` repeats the first.
export interface AskPending extends PendingBase {
  kind: "ask";
  questions?: AskQuestion[];
}
export interface ApprovalPending extends PendingBase {
  kind: "approval";
  approval?: {
    tool: string;
    mandatory_human?: boolean;
    mandatory_reason?: string;
    preview?: string;
    // What approving risks, most serious first; then the reasons the
    // automatic review gave, by name and probability.
    risks?: { id: string; label: string }[];
    // What the automatic review made of the call and which bar it fell on.
    review?: string;
  };
}
// A line of text, a file to edit, or a step done in the browser.
export interface InputPending extends PendingBase {
  kind: "text" | "editor" | "browser" | "project.trust" | "model.select";
}
export type Pending = AskPending | ApprovalPending | InputPending;
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
export interface Permissions {
  mode: string;
  default: string;
  effective?: string;
  // The mode no choice here can exceed, where the conversation has one.
  limit?: string;
}
interface PermissionRule {
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
  // A folder's panel, or a thread it launched (in `folder`), and the plan step
  // the thread works for.
  kind?: "panel" | "thread" | "";
  folder?: string;
  step?: string;
  // The model its file names.
  model?: string;
  status?: SessionStatus;
  presence?: "active" | "";
  updated?: number;
  incoming?: number;
  turn_active?: boolean;
  pending?: boolean;
  // What it waits on you for: "approval", "ask", …, and its prompt.
  pending_prompt?: string;
  turn_status?: string;
  activities?: Activity[];
  guidance?: number;
  error?: string;
}
export type SessionRef = Pick<Session, "id" | "generation">;
export interface State {
  phase?: ExecutionPhase;
  // When the running turn started, on the host's clock.
  turn_started_ms?: number;
  attachments?: number;
  title?: string;
  route?: string;
  effort?: string;
  efforts?: string[];
  view?: View;
  // Bumped by an in-place rewind, which replaces the view.
  view_epoch?: number;
  turns?: number;
  usage?: Usage;
  system_prompt?: string;
  // Why a panel holds its threads' events (today's spend limit).
  paused?: string;
  statistics?: Statistics;
  turn_status?: string;
  activities?: Activity[];
  agents?: Agent[];
  // A panel's plan.
  plan?: Plan;
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
  // Navigation a client performs itself; a runtime refuses it.
  client_only?: boolean;
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
  // The level names from least detail to most.
  order: string[];
}
// A workspace: a folder the host keeps for conversations that belong to no
// folder, under the name its user gave it.
export interface Space {
  path: string;
  name: string;
}

export interface Catalogue {
  spaces?: Space[];
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
interface EventData extends Omit<Partial<Exchange>, "status"> {
  request_id?: string;
  // approval.requested: "panel" while a thread's panel decides.
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
  parts?: ToolPart[];
  status?: string | number;
  duration_ms?: number;
  activity?: ToolActivity;
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
export interface BlockPatch {
  kind: "block";
  block?: Block;
  id?: string;
  set?: Pick<AssistantBlock, "streaming">;
  append?: { text?: string; reasoning?: string };
}
// Host envelopes and native EventEmitter payloads share the same SSE channel.
interface HostEnvelope extends Partial<Omit<Outcome, "pending">> {
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
  turn_status?: string;
}
export type HostEvent = HostEnvelope &
  (
    | ({ kind: "outcome" } & Outcome)
    | { kind: "state"; pending?: Pending | null }
    | BlockPatch
    | {
        kind:
          | "event"
          | "turn_status"
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
    | { kind: "settings.changed"; level: string }
    | { kind: "spaces.changed"; spaces: Space[] }
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
}
export interface ModelCatalogue {
  models: Model[];
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
  category: string;
  type: string;
  sensitivity: string;
  takes_effect: string;
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
  // The groups settings are listed under, in order.
  categories: { id: string; label: string }[];
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
// One instruction file a person edits: for every conversation or a folder's
// panel, yours or the folder's.
export interface InstructionFile {
  audience: "sessions" | "panel";
  scope: "user" | "project";
  path: string;
  text: string;
}
export interface InstructionStack {
  files: InstructionFile[];
  also_loaded: string[];
  base: { sessions: string; panel: string };
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
  config: Configuration;
  restart_conversations: { restarting: number; deferred: number };
  space: { spaces: Space[] };
  restart_host: { restarting: boolean };
  tools: ToolCatalogue;
  activity: ActivityDetail;
  context: { exchanges: Exchange[] };
  revert: { restored: string[]; conflicts: { path: string; reason: string }[] };
  // A fork cut before a message returns that message, to edit (prompt).
  // A panel rewinds itself in place (`rewound`) instead of forking.
  fork: { id: string; prompt?: string; rewound?: boolean };
  share: { path: string };
  side: { id: string };
  create: never;
  activate: never;
  close: never;
  rename: never;
  delete: never;
  logout: never;
  submit: never;
  steer: never;
  recall: never;
  insert: never;
  reply: never;
  interrupt: never;
  permissions: Permissions;
  permission_rules: PermissionRules;
  revoke_device: never;
  test_notification: never;
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
  };
  instructions: {
    action: string;
    cwd?: string;
    audience?: string;
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
  restart_host: None;
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
    agent_id?: string;
    text?: string;
    model?: string;
    before?: number;
    detail?: string;
    raw?: boolean;
    offset?: number;
  };
  context: None;
  // The turn (0 = latest) and one file of it, or all.
  revert: { turn: number; path?: string };
  // argument: raw text after /fork, parsed by the native host; message_id:
  // the message a fork is cut before ("m-<id>").
  fork: { argument?: string; message_id?: string };
  share: None;
  side: { operation: string; text: string };
  // A workspace by its name (`space`), in place of `cwd`.
  create: { cwd?: string; space?: string; panel?: boolean };
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
  permissions: { mode: string };
  permission_rules: { action: string; key?: string; cwd?: string };
  revoke_device: { device_id: string };
  test_notification: None;
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
      block?: never;
      snapshot?: never;
    }
  // A workspace, by its name; deleting says how many conversations go with it.
  | {
      type: "rename" | "delete";
      space: Space;
      conversations?: number;
      session?: never;
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
  | { type: "new"; place: "workspace" | "folder" }
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
  imported?: { kind: string; name: string }[];
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
  id: string;
  revision: string;
  name: string;
  prompt: string;
  cwd: string;
  model: string;
  permissions: "ask" | "auto" | "yolo";
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
  | { block: ActivityTarget }
  | { item: Activity }
  | { raw: RawOptions; title: string };
