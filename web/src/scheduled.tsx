import type { ScheduledTask, ScheduledState, ScheduleRule } from "./types.ts";
import { useEffect, useState } from "preact/hooks";
import { Plus, ArrowLeft, Play, Square } from "lucide-preact";
import { Field, Select, Modal, LoadError } from "./ui.tsx";
import { Menu, MenuItem } from "./popover.tsx";
import { Popover } from "./popover.tsx";
import ModelPicker from "./model-picker.tsx";
import { readStored, writeStored } from "./store.ts";
import {
  manage,
  ProjectField,
  ManagementSkeleton,
  dateTime,
  taskActive,
} from "./management.tsx";

const blank = (cwd: string): ScheduledTask => ({
  id: "",
  revision: "",
  name: "",
  prompt: "",
  cwd,
  model: "",
  permissions: "prompt",
  environment: "worktree",
  enabled: true,
  schedule: {
    type: "weekly",
    time: "09:00",
    days: [1, 2, 3, 4, 5],
    timezone: Intl.DateTimeFormat().resolvedOptions().timeZone,
  },
});
const storage = "uagent-scheduled-draft";
function localInput(at?: number) {
  const value = new Date((at || Date.now() / 1000 + 3600) * 1000);
  return new Date(value.getTime() - value.getTimezoneOffset() * 60000)
    .toISOString()
    .slice(0, 16);
}
export default function Scheduled({
  projects,
  cwd,
  online,
  scheduled,
  unread,
  choose,
  refresh,
}: {
  projects: string[];
  cwd: string;
  online: boolean;
  scheduled?: ScheduledState;
  unread: Set<string>;
  choose: (id: string) => Promise<void>;
  refresh: () => Promise<void>;
}) {
  const [task, setTask] = useState<ScheduledTask | null>(() =>
    readStored(sessionStorage, storage, null),
  );
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [confirm, setConfirm] = useState(false);
  const [times, setTimes] = useState<number[] | null>(null);
  const [previewError, setPreviewError] = useState<unknown>(null);
  const current = scheduled?.tasks?.find((entry) => entry.id === task?.id);
  const changed =
    !!task?.id && (!current || task.revision !== current.revision);
  const definition = (value?: ScheduledTask | null) =>
    value
      ? JSON.stringify([
          value.name,
          value.prompt,
          value.cwd,
          value.model,
          value.permissions,
          value.environment,
          value.schedule,
          value.enabled,
        ])
      : "";
  const dirty = task && definition(task) !== definition(current);
  const scheduleKey = JSON.stringify(task?.schedule);
  useEffect(() => {
    if (task) {
      writeStored(sessionStorage, storage, task);
      if (dirty)
        writeStored(sessionStorage, `${storage}-${task.id || "new"}`, task);
    } else sessionStorage.removeItem(storage);
  }, [task]);
  useEffect(() => {
    let active = true;
    setTimes(null);
    setPreviewError(null);
    if (!task || !online) return;
    const timer = setTimeout(
      () =>
        manage("schedule", { action: "preview", schedule: task.schedule })
          .then((result) => {
            if (active) setTimes(result.times || []);
          })
          .catch((failure) => {
            if (active) setPreviewError(failure);
          }),
      200,
    );
    return () => {
      active = false;
      clearTimeout(timer);
    };
  }, [scheduleKey, online]);
  function chooseTask(value: ScheduledTask | null) {
    setTask(
      value
        ? readStored<ScheduledTask>(
            sessionStorage,
            `${storage}-${value.id || "new"}`,
            value,
          )
        : null,
    );
    setError(null);
  }
  function discard() {
    sessionStorage.removeItem(`${storage}-${task?.id || "new"}`);
    setTask(current || null);
  }
  const edit = (value: Partial<ScheduledTask>) =>
    setTask((old) => (old ? { ...old, ...value } : old));
  const rule = (value: Partial<ScheduleRule>) => {
    if (task) edit({ schedule: { ...task.schedule, ...value } });
  };
  async function action(action: string, key = task?.id) {
    setBusy(true);
    setError(null);
    try {
      const result = await manage("schedule", {
        action,
        key,
        revision: task?.revision,
        ...(action === "save" && task ? { task } : {}),
      });
      if (result.item) {
        sessionStorage.removeItem(`${storage}-${task?.id || "new"}`);
        setTask(result.item);
      }
      if (action === "forget") {
        setTask(null);
        setConfirm(false);
      }
      await refresh();
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  }
  const runs = [...(scheduled?.runs || [])]
    .reverse()
    .filter((run) => !task?.id || run.task_id === task.id);
  return (
    <div class="management">
      <div class="management-toolbar">
        <p class="muted">
          Runs while the uAgent host is open. Your browser can close.
        </p>
        <button
          class="primary with-icon"
          disabled={!online || busy}
          onClick={() => {
            chooseTask(blank(cwd || projects[0] || ""));
          }}
        >
          <Plus />
          New task
        </button>
      </div>
      {scheduled?.error ? (
        <LoadError error={scheduled.error} retry={refresh} />
      ) : !scheduled ? (
        <ManagementSkeleton />
      ) : (
        <div class={`management-body ${task ? "has-selection" : ""}`}>
          <div class="management-list">
            <button
              class={`library-row ${!task ? "selected" : ""}`}
              onClick={() => setTask(null)}
            >
              All runs
            </button>
            {scheduled.tasks.map((entry) => (
              <button
                key={entry.id}
                class={`library-row ${task?.id === entry.id ? "selected" : ""}`}
                disabled={busy}
                onClick={() => {
                  chooseTask(entry);
                }}
              >
                <span>{entry.name}</span>
                <small>
                  {entry.enabled
                    ? dateTime(entry.next, entry.schedule.timezone)
                    : "Paused"}
                  {scheduled.runs.some(
                    (run) =>
                      run.task_id === entry.id && unread.has(run.session_id),
                  ) && <span class="unread-dot" aria-label="Unread results" />}
                </small>
              </button>
            ))}
            {!scheduled.tasks.length && (
              <p class="muted">
                Schedule a recurring review or a one-off task.
              </p>
            )}
          </div>
          <fieldset disabled={busy} class="management-editor schedule-editor">
            {task && (
              <>
                <div class="editor-head">
                  <button class="quiet with-icon" onClick={() => setTask(null)}>
                    <ArrowLeft />
                    All runs
                  </button>
                  <strong>{task.id ? task.name : "New task"}</strong>
                  {task.id && (
                    <Menu label="Task menu">
                      <MenuItem
                        disabled={!online || busy}
                        onClick={() =>
                          action(task.enabled ? "pause" : "resume")
                        }
                      >
                        {task.enabled ? "Pause" : "Resume"}
                      </MenuItem>
                      <MenuItem
                        disabled={busy}
                        onClick={() => setConfirm(true)}
                      >
                        Delete task
                      </MenuItem>
                    </Menu>
                  )}
                </div>
                {changed && (
                  <p role="status">
                    This task changed. Your draft is retained.{" "}
                    <button onClick={discard}>Reload</button>
                  </p>
                )}
                <div class="field-row">
                  <Field label="Name">
                    <input
                      value={task.name}
                      onInput={(event) =>
                        edit({ name: event.currentTarget.value })
                      }
                    />
                  </Field>
                  <ProjectField
                    value={task.cwd}
                    projects={projects}
                    change={(cwd) => edit({ cwd })}
                  />
                </div>
                <Field label="Instructions">
                  <textarea
                    rows={5}
                    value={task.prompt}
                    onInput={(event) =>
                      edit({ prompt: event.currentTarget.value })
                    }
                    placeholder="What should uAgent do, and what result should it report?"
                  />
                </Field>
                <div class="field-row">
                  <Field label="Repeat">
                    <Select
                      value={task.schedule.type}
                      onChange={(event) => {
                        const type = event.currentTarget
                          .value as ScheduleRule["type"];
                        edit({
                          schedule:
                            type === "weekly"
                              ? blank(task.cwd).schedule
                              : type === "interval"
                                ? { type, seconds: 3600 }
                                : {
                                    type,
                                    at: Math.floor(Date.now() / 1000) + 3600,
                                  },
                        });
                      }}
                    >
                      <option value="weekly">Selected weekdays</option>
                      <option value="interval">Fixed interval</option>
                      <option value="once">Once</option>
                    </Select>
                  </Field>
                  {task.schedule.type === "once" ? (
                    <Field label="Date and time · your timezone">
                      <input
                        type="datetime-local"
                        value={localInput(task.schedule.at)}
                        onChange={(event) =>
                          rule({
                            at: Math.floor(
                              new Date(event.currentTarget.value).getTime() /
                                1000,
                            ),
                          })
                        }
                      />
                    </Field>
                  ) : task.schedule.type === "interval" ? (
                    <Field label="Every · minutes">
                      <input
                        type="number"
                        min={1}
                        max={525600}
                        value={(task.schedule.seconds || 3600) / 60}
                        onInput={(event) =>
                          rule({
                            seconds: Number(event.currentTarget.value) * 60,
                          })
                        }
                      />
                    </Field>
                  ) : (
                    <>
                      <Field label="Time">
                        <input
                          type="time"
                          value={task.schedule.time}
                          onChange={(event) =>
                            rule({ time: event.currentTarget.value })
                          }
                        />
                      </Field>
                      <Field label="Timezone">
                        <input
                          value={task.schedule.timezone}
                          onChange={(event) =>
                            rule({ timezone: event.currentTarget.value })
                          }
                          placeholder="Europe/Zurich"
                        />
                      </Field>
                    </>
                  )}
                </div>
                {task.schedule.type === "weekly" && (
                  <div class="weekdays" role="group" aria-label="Weekdays">
                    {["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].map(
                      (day, index) => (
                        <button
                          aria-pressed={task.schedule.days?.includes(index)}
                          onClick={() =>
                            rule({
                              days: task.schedule.days?.includes(index)
                                ? task.schedule.days.filter(
                                    (value) => value !== index,
                                  )
                                : [...(task.schedule.days || []), index],
                            })
                          }
                        >
                          {day}
                        </button>
                      ),
                    )}
                  </div>
                )}
                <p class="muted" aria-live="polite">
                  {previewError
                    ? "Choose a valid schedule to see upcoming runs."
                    : times
                      ? times.length
                        ? `Next: ${times.map((at) => dateTime(at, task.schedule.timezone)).join(" · ")}`
                        : "No future run. Choose a later date."
                      : online
                        ? "Calculating upcoming runs…"
                        : "Reconnect to preview."}
                </p>
                {previewError && <LoadError error={previewError} />}
                <div class="field-row">
                  <Field
                    label="Environment"
                    help={
                      task.environment === "worktree"
                        ? "Starts from committed HEAD; changes are kept for review."
                        : "Changes apply directly to this project."
                    }
                  >
                    <Select
                      aria-label="Environment"
                      value={task.environment}
                      onChange={(event) =>
                        edit({
                          environment: event.currentTarget
                            .value as ScheduledTask["environment"],
                        })
                      }
                    >
                      <option value="worktree">Git worktree</option>
                      <option value="local">Local project</option>
                    </Select>
                  </Field>
                  <Field
                    label="Permissions"
                    help={
                      task.permissions === "prompt"
                        ? "Pauses for approval in the run's conversation."
                        : "Runs without ordinary approval prompts."
                    }
                  >
                    <Select
                      aria-label="Permissions"
                      value={task.permissions}
                      onChange={(event) =>
                        edit({
                          permissions: event.currentTarget
                            .value as ScheduledTask["permissions"],
                        })
                      }
                    >
                      <option value="prompt">Ask</option>
                      <option value="yolo">YOLO</option>
                    </Select>
                  </Field>
                </div>
                <Popover
                  buttonClass="quiet"
                  align="start"
                  label="Task model"
                  title="Model, variant and effort"
                  trigger={
                    <span>{task.model || "Model · project default"}</span>
                  }
                >
                  {(close) => (
                    <ModelPicker
                      session={{ id: task.id || "new-task", cwd: task.cwd }}
                      selection={task.model}
                      save={(model) => edit({ model })}
                      close={close}
                      online={online}
                      running={false}
                    />
                  )}
                </Popover>
                <div class="editor-actions">
                  <button disabled={busy} onClick={discard}>
                    Cancel
                  </button>
                  {task.id && (
                    <button
                      class="with-icon"
                      disabled={
                        !online ||
                        busy ||
                        !!dirty ||
                        scheduled.runs.some(
                          (run) =>
                            run.task_id === task.id && taskActive(run.status),
                        )
                      }
                      onClick={() => action("run")}
                    >
                      <Play />
                      Run now
                    </button>
                  )}
                  <button
                    class="primary"
                    disabled={
                      !online ||
                      busy ||
                      !dirty ||
                      !task.name.trim() ||
                      !task.prompt.trim() ||
                      !task.cwd ||
                      changed ||
                      !times?.length
                    }
                    onClick={() => action("save")}
                  >
                    {busy ? "Saving…" : "Save"}
                  </button>
                </div>
              </>
            )}
            {error && <LoadError error={error} />}
            <div class="run-history">
              <h2>{task ? "Run history" : "All runs"}</h2>
              {!runs.length ? (
                <p class="muted">
                  Results appear here. Missed runs are skipped; interrupted runs
                  wait for your review.
                </p>
              ) : (
                runs.map((run) => (
                  <div class="run-row" key={run.id}>
                    <button
                      disabled={
                        run.session_available === false ||
                        ["queued", "starting", "missed", "skipped"].includes(
                          run.status,
                        )
                      }
                      onClick={() => choose(run.session_id).catch(setError)}
                    >
                      <span>
                        {run.title}
                        {unread.has(run.session_id) && (
                          <span
                            class="unread-dot"
                            aria-label="Unread results"
                          />
                        )}
                      </span>
                      <small>
                        {run.status} · {dateTime(run.scheduled_for)}
                      </small>
                      {run.error && <small>{run.error}</small>}
                    </button>
                    {taskActive(run.status) && (
                      <button
                        class="quiet icon-button"
                        aria-label="Stop run"
                        disabled={!online || busy || run.status === "stopping"}
                        onClick={() => action("stop", run.id)}
                      >
                        <Square />
                      </button>
                    )}
                  </div>
                ))
              )}
            </div>
          </fieldset>
        </div>
      )}
      {confirm && task && (
        <Modal title={`Delete ${task.name}?`} close={() => setConfirm(false)}>
          <p>
            Future runs will stop. Existing conversations and worktrees are
            kept.
          </p>
          {error && <LoadError error={error} />}
          <div class="dialog-actions">
            <button onClick={() => setConfirm(false)}>Cancel</button>
            <button
              class="primary"
              disabled={busy || !online}
              onClick={() => action("forget")}
            >
              Delete
            </button>
          </div>
        </Modal>
      )}
    </div>
  );
}
