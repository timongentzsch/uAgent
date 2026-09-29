import type { Pending, Act, Report } from "../../shared/types.ts";
import { useState } from "preact/hooks";
import {
  Actions,
  Button,
  cleanText,
  Select,
  Input,
  Textarea,
} from "../../shared/ui.tsx";
import Ask from "./ask.tsx";

export default function Decision({
  pending,
  session,
  act,
  online,
  report,
}: {
  pending: Pending;
  session: string;
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
  const asking = pending.kind === "ask";
  const send = async (text: string, attachment_ids?: string[]) => {
    setSending(true);
    try {
      await act("reply", { interaction_id: pending.id, text, attachment_ids });
    } catch (failure) {
      report(failure);
      setSending(false);
    }
  };
  const cancel = () =>
    act("reply", {
      interaction_id: pending.id,
      text: "",
      cancelled: true,
    }).catch(report);
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
      <h2>
        {pending.route === "coordinator"
          ? "The coordinator is deciding"
          : asking
            ? "Needs your answer"
            : "Needs your decision"}
      </h2>
      {pending.route === "coordinator" && (
        <p class="decision-note">You can still answer first.</p>
      )}
      {pending.note && <p class="decision-note">{cleanText(pending.note)}</p>}
      {!asking && (
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
      )}
      {asking ? (
        <Ask
          id={pending.id}
          session={session}
          questions={pending.questions ?? []}
          online={online}
          sending={sending}
          send={(text, ids) => void send(text, ids)}
          cancel={cancel}
          report={report}
        />
      ) : keyed ? (
        <form
          onSubmit={(event) => {
            event.preventDefault();
            void send(guidance);
          }}
        >
          <Actions>
            {options.map((item) => (
              <Button
                variant={item.value === "y" ? "primary" : "secondary"}
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
              </Button>
            ))}
          </Actions>
          {reply === "guidance" && (
            <>
              {guidanceInput}
              <Button
                type="submit"
                variant="primary"
                disabled={!online || sending}
              >
                Send guidance
              </Button>
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
              Your text
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
          <Actions>
            <Button onClick={cancel} disabled={!online}>
              Cancel
            </Button>
            <Button
              type="submit"
              variant="primary"
              disabled={!online || sending || !reply}
            >
              Send response
            </Button>
          </Actions>
        </form>
      )}
    </section>
  );
}
