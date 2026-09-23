// The native host owns this versioned wire contract. Unknown JSON stays at the
// HTTP boundary; components share these projections instead of redefining them.
export type JSONValue =
  null | boolean | number | string | JSONValue[] | { [key: string]: JSONValue };
export type Report = (error: unknown) => void;
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
  usage_samples?: number;
  side_recorded_turns?: number;
  side_tool_calls?: number;
  side_model_calls?: number;
  side_duration_ms?: number;
  side_model_ms?: number;
  side_tool_ms?: number;
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
  reply_to?: string;
  reply_excerpt?: string;
}
export interface ToolActivity {
  category?: "explore" | "change" | "execute";
  label?: string;
  group?: { id: string; label: string };
}
export interface ToolReplay {
  title?: string;
  summary?: string;
  poll?: boolean;
  multiline?: boolean;
  detail?: string;
}
export interface ToolCall {
  activity?: ToolActivity;
  replay?: ToolReplay;
  id?: string;
  response_id?: string;
  occurrence_id?: string;
  call_id?: string;
  detail_id?: string;
  name: string;
  arguments?: JSONValue;
  status?: string;
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
}
export interface Block {
  memory?: { action: string; key: string; automatic: boolean };
  activity?: ToolActivity;
  sequence?: number;
  summary?: TurnSummary;
  compaction?: {
    automatic: boolean;
    messages_before: number;
    messages_after: number;
    retained_user_messages: number;
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
  reasoning_revision?: number;
  reasoning_complete?: boolean;
  reasoning_bytes?: number;
  retained_reasoning_bytes?: number;
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
  unavailable_images?: number;
  tools?: ToolCall[];
  call_id?: string;
  name?: string;
  arguments?: JSONValue;
  detail_id?: string;
  status?: string;
  error?: string;
  duration_ms?: number;
  // Retained tool_result whose receipt facts are gone: its fallbacks
  // ("tool"/"running") must never shadow the call record it joins.
  receipt_missing?: boolean;
  replay?: ToolReplay;
  ttft_ms?: number;
  tokens_per_second?: number;
  route?: string;
  change?: string;
  truncated?: boolean;
  usage?: Usage;
  usage_reported?: boolean;
  http?: Exchange[];
  turn_root?: string;
  reply_to?: string;
  reply_excerpt?: string;
  activity_id?: number;
  agent_id?: string;
  command?: string;
}
export interface PresentedBlock extends Block {
  children?: PresentedBlock[];
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
}
export interface Collaborator {
  id: string;
  label?: string;
  name?: string;
  description?: string;
  team?: string;
  model?: string;
  status?: string;
  persistent?: boolean;
}
export interface ActivityDetail extends Activity {
  statistics_live?: boolean;
  route?: string;
  persistent?: boolean;
  olderWindow?: boolean;
  communication?: { from: string; to: string; text: string; time: string }[];
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
  receipt?: string;
}
export interface Pending {
  id: string;
  kind?: string;
  initial?: string;
  prompt?: string;
  options?: (string | { value: string; label?: string; title?: string })[];
  approval?: { tool: string; mandatory_human?: boolean; preview?: string };
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
export interface Session {
  task_id?: string;
  run_id?: string;
  id: string;
  generation?: string;
  title?: string;
  cwd?: string;
  status?: string;
  presence?: "active" | "";
  updated?: number;
  incoming?: number;
  turn_active?: boolean;
  pending?: boolean;
  activity?: string;
  activities?: Activity[];
  guidance?: number;
  error?: string;
}
export type SessionRef = Pick<Session, "id" | "generation">;
export interface State {
  phase?: ExecutionPhase;
  pending_decision?: Pending | null;
  attachments?: number;
  title?: string;
  route?: string;
  effort?: string;
  efforts?: string[];
  variant?: string;
  variants?: string[];
  view?: View;
  turns?: number;
  usage?: Usage;
  route_usage?: Record<string, Usage>;
  system_prompt?: string;
  statistics?: Statistics;
  activity?: string;
  activities?: Activity[];
  collaborators?: Collaborator[];
  context_tokens?: number;
  context_window?: number;
  permissions?: Permissions;
  http?: Exchange[];
  error?: string;
}
export type ExecutionPhase =
  | "idle"
  | "working"
  | "waiting"
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
  streamed?: Block[];
  live_truncated?: boolean;
}
export interface SlashCommand {
  command: string;
  argument: string;
  aliases: string[];
  usage: string;
  description: string;
}
export interface Catalogue {
  commands?: SlashCommand[];
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
  inspect?: boolean;
  context_tokens?: number;
  output?: string;
  usage?: Usage;
  statistics?: Statistics;
  block?: Block;
  text?: string;
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
  result?: JSONValue;
  preview_truncated?: boolean;
  completion_status?: string;
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
  collaborator?: Collaborator;
  removed?: boolean;
  permissions?: Permissions;
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
  data?: EventData;
  metadata?: Session;
  state?: State;
  phase?: ExecutionPhase;
  pending_decision?: Pending | null;
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
export interface ConfigSetting {
  name: string;
  description: string;
  category: string;
  type: string;
  sensitivity: string;
  scopes: string[];
  value?: JSONValue;
  active?: JSONValue;
  default?: JSONValue;
  source: string;
  takes_effect: string;
  set?: boolean;
  minimum?: number;
  maximum?: number;
}
export interface ConfigChange {
  key: string;
  value?: string;
  unset?: boolean;
}
export interface Configuration {
  settings: ConfigSetting[];
  project_trusted?: boolean;
  effects: { key: string; effect: string }[];
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
export interface CommandResults {
  browser: {
    running?: boolean;
    mode?: string;
    created_profile_id?: string;
  };
  prompt: PromptResult;
  memory: LibraryResult;
  skills: LibraryResult;
  schedule: ScheduleResult;
  models: ModelCatalogue;
  model: ModelCatalogue;
  config: Configuration;
  tools: ToolCatalogue;
  tool_categories: ToolCategories;
  activity: ActivityDetail;
  context: { exchanges: Exchange[] };
  fork: { id: string };
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
    };
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
  | { type: "browser" }
  | { type: "prompt"; scope?: string; edit?: boolean }
  | StatisticsModal
  | ({ type: "raw" } & RawOptions)
  | { type: "new" | "settings" }
  | { type: "tools"; session_id: string };

export interface PromptDocument {
  scope: string;
  mode: "inherit" | "overlay" | "replace";
  text: string;
  revision: string;
  path?: string;
  active?: boolean;
}
export interface PromptResult {
  item: PromptDocument;
  effective: string;
  last_sent?: string;
  inherited: Record<string, string>;
  sources: PromptDocument[];
  bytes: number;
  digest: string;
  diff?: string;
  applies?: string;
  preview_kind?: string;
}

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
  task_revision: string;
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
  runner_active?: boolean;
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
