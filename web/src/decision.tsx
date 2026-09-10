import type { Pending, Act, Report } from "./types.ts";
import { useState } from "preact/hooks";
import { cleanText, Select } from "./ui.tsx";

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
  const [reply, setReply] = useState(
    pending.options?.some(
      (item) => (typeof item === "string" ? item : item.value) === "n",
    )
      ? "n"
      : "",
  );
  const [guidance, setGuidance] = useState("");
  const [sending, setSending] = useState(false);
  const approval = pending.approval;
  return (
    <section class="decision" aria-label="Pending decision">
      <h2>Needs your decision</h2>
      <div class="decision-preview">
        {approval && (
          <>
            <strong>
              {approval.tool}
              {approval.mandatory_human ? " · explicit approval required" : ""}
            </strong>
            <pre>{cleanText(approval.preview)}</pre>
          </>
        )}
        {approval && pending.options?.length ? (
          <details>
            <summary>Details</summary>
            <p>{cleanText(pending.prompt)}</p>
          </details>
        ) : (
          <p>{cleanText(pending.prompt)}</p>
        )}
      </div>
      <form
        onSubmit={async (event) => {
          event.preventDefault();
          setSending(true);
          try {
            await act("reply", {
              interaction_id: pending.id,
              text: reply === "guidance" ? guidance : reply,
            });
          } catch (failure) {
            report(failure);
            setSending(false);
          }
        }}
      >
        {!!pending.options?.length ? (
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
              {pending.options.map((item) => (
                <option value={typeof item === "string" ? item : item.value}>
                  {typeof item === "string"
                    ? item
                    : item.label || item.title || item.value}
                </option>
              ))}
            </Select>
          </label>
        ) : (
          <label>
            Response
            <input
              aria-label="Response"
              name="reply"
              autoComplete="off"
              value={reply}
              onInput={(event) => setReply(event.currentTarget.value)}
            />
          </label>
        )}
        {reply === "guidance" && (
          <label>
            Guidance
            <input
              value={guidance}
              onInput={(event) => setGuidance(event.currentTarget.value)}
              required
            />
          </label>
        )}
        <button disabled={!online || sending || !reply}>Send response</button>
        <button
          type="button"
          onClick={() => act("interrupt").catch(report)}
          disabled={!online}
        >
          Cancel
        </button>
      </form>
    </section>
  );
}
