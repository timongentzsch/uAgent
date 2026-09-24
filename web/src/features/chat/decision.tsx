import type { Pending, Act, Report } from "../../shared/types.ts";
import { useState } from "preact/hooks";
import { cleanText, Select, Input, Textarea } from "../../shared/ui.tsx";

export default function Decision({
  pending,
  act,
  online,
  report,
}: {
  pending: Pending;
  act: Act;
  online: boolean;
  report: Report;
}) {
  const options = (pending.options ?? []).map((item) =>
    typeof item === "string"
      ? { value: item, label: item }
      : { value: item.value, label: item.label || item.title || item.value },
  );
  // Letter-keyed answers are one choice each, as the terminal's key hints
  // are; numbered lists stay a dropdown.
  const keyed =
    options.length > 0 &&
    options.every(
      (item) => item.value === "guidance" || /^[a-z]$/i.test(item.value),
    );
  const [reply, setReply] = useState(
    options.some((item) => item.value === "n") ? "n" : pending.initial || "",
  );
  const [guidance, setGuidance] = useState("");
  const [sending, setSending] = useState(false);
  const approval = pending.approval;
  const send = async (text: string) => {
    setSending(true);
    try {
      await act("reply", { interaction_id: pending.id, text });
    } catch (failure) {
      report(failure);
      setSending(false);
    }
  };
  const guidanceInput = (
    <label>
      Guidance
      <Input
        value={guidance}
        onInput={(event) => setGuidance(event.currentTarget.value)}
        required
      />
    </label>
  );
  return (
    <section class="decision" aria-label="Pending decision">
      <h2>Needs your decision</h2>
      <div class="decision-preview">
        {approval && (
          <>
            <strong>
              {approval.tool}
              {approval.mandatory_human
                ? ` · ${approval.mandatory_reason || "explicit approval required"}`
                : ""}
            </strong>
            <pre>{cleanText(approval.preview)}</pre>
          </>
        )}
        <p>{cleanText(pending.prompt)}</p>
      </div>
      {keyed ? (
        <form
          onSubmit={(event) => {
            event.preventDefault();
            void send(guidance);
          }}
        >
          <div class="dialog-actions">
            {options.map((item) => (
              <button
                type="button"
                class={item.value === "y" ? "primary" : undefined}
                aria-pressed={
                  item.value === "guidance" ? reply === "guidance" : undefined
                }
                disabled={!online || sending}
                onClick={() =>
                  item.value === "guidance"
                    ? setReply("guidance")
                    : void send(item.value)
                }
              >
                {item.label}
              </button>
            ))}
          </div>
          {reply === "guidance" && (
            <>
              {guidanceInput}
              <button class="primary" disabled={!online || sending}>
                Send guidance
              </button>
            </>
          )}
        </form>
      ) : (
        <form
          onSubmit={(event) => {
            event.preventDefault();
            void send(reply);
          }}
        >
          {pending.kind === "editor" ? (
            <label>
              System prompt
              <Textarea
                rows={12}
                value={reply}
                onInput={(event) => setReply(event.currentTarget.value)}
              />
            </label>
          ) : options.length ? (
            <label>
              Response
              <Select
                aria-label="Response"
                name="reply"
                value={reply}
                onChange={(event) => setReply(event.currentTarget.value)}
              >
                <option value="" disabled>
                  Select a response
                </option>
                {options.map((item) => (
                  <option value={item.value}>{item.label}</option>
                ))}
              </Select>
            </label>
          ) : (
            <label>
              Response
              <Input
                aria-label="Response"
                name="reply"
                autoComplete="off"
                value={reply}
                onInput={(event) => setReply(event.currentTarget.value)}
              />
            </label>
          )}
          <div class="dialog-actions">
            <button
              type="button"
              onClick={() =>
                act("reply", {
                  interaction_id: pending.id,
                  text: "",
                  cancelled: true,
                }).catch(report)
              }
              disabled={!online}
            >
              Cancel
            </button>
            <button class="primary" disabled={!online || sending || !reply}>
              Send response
            </button>
          </div>
        </form>
      )}
    </section>
  );
}
