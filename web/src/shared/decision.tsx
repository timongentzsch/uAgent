import type { Pending, Act, CommandInputs, Report } from "./types.ts";
import { failure } from "./types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import {
  Actions,
  Button,
  cleanText,
  Input,
  InputRow,
  Notice,
  Textarea,
} from "./ui.tsx";
import Ask from "./ask.tsx";
import { AttachmentList } from "./attachments.tsx";
import DiffView from "./diff-view.tsx";
import { useApi } from "../state/api.ts";
import "./decision.css";

// The approval's own answers (the host's choice table); anything else it
// offers shows as a plain button after them.
const ALLOW_ONCE = "y",
  ALLOW_SESSION = "s",
  ALWAYS = "a",
  NEVER = "x",
  DENY = "n",
  GUIDANCE = "guidance";
const CARD = new Set([
  ALLOW_ONCE,
  ALLOW_SESSION,
  ALWAYS,
  NEVER,
  DENY,
  GUIDANCE,
]);
// What the folder keeps once the person answers: nothing, or a rule that
// allows (sent with an allowing answer) or refuses (with Deny).
type Remember = "" | "always" | "never";
type Answer = Omit<CommandInputs["reply"], "interaction_id">;

export default function Decision({
  pending,
  session,
  cwd,
  act,
  online,
  report,
}: {
  pending: Pending;
  session: string;
  cwd?: string;
  act: Act;
  online: boolean;
  report: Report;
}) {
  const { routes } = useApi();
  const options = (pending.options ?? []).map((item) =>
    typeof item === "string"
      ? { value: item, label: item }
      : { value: item.value, label: item.label || item.title || item.value },
  );
  const offers = (value: string) =>
    options.some((item) => item.value === value);
  const labelOf = (value: string) =>
    options.find((item) => item.value === value)?.label ?? value;
  // Letter-keyed answers are one choice each, as the terminal's key hints
  // are; numbered lists are radio cards.
  const keyed =
    options.length > 0 &&
    options.every(
      (item) => item.value === GUIDANCE || /^[a-z]$/i.test(item.value),
    );
  const [reply, setReply] = useState(
    offers(DENY) ? DENY : pending.initial || "",
  );
  const [guidance, setGuidance] = useState("");
  const [remember, setRemember] = useState<Remember>("");
  const [sending, setSending] = useState(false);
  const [failed, setFailed] = useState<{
    message: string;
    fields: Answer;
  } | null>(null);
  const approval = pending.kind === "approval" ? pending.approval : undefined;
  const asking = pending.kind === "ask";
  // One reply in flight at a time: answer and cancel share the guard.
  const answer = async (fields: Answer) => {
    setSending(true);
    setFailed(null);
    try {
      await act("reply", { interaction_id: pending.id, ...fields });
    } catch (error) {
      report(error, "inline");
      setFailed({ message: failure(error).message, fields });
    } finally {
      setSending(false);
    }
  };
  const send = (text: string, attachment_ids?: string[]) =>
    answer({ text, attachment_ids });
  const cancel = () => answer({ text: "", cancelled: true });
  const idle = !online || sending;
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
  // Each new decision takes focus, unless someone is typing elsewhere. The
  // composer's input, held read-only below a pending decision, does not count.
  const heading = useRef<HTMLHeadingElement>(null);
  useEffect(() => {
    const focused = document.activeElement;
    const typing =
      focused instanceof HTMLElement &&
      (focused.isContentEditable ||
        (focused.matches("input:not([readonly]), textarea:not([readonly])") &&
          focused.isConnected));
    if (!typing) heading.current?.focus({ preventScroll: true });
  }, []);
  const title =
    pending.route === "parent"
      ? "The parent agent is deciding"
      : asking
        ? "Needs your answer"
        : "Needs your decision";
  const preview = cleanText(approval?.preview);
  // A file change previews as its diff: a header line ("Edited a.ts (+2
  // -1)"), then hunks or added lines.
  const diff =
    /^[+-]/m.test(preview) &&
    (/^@@ /m.test(preview) || /\(\+\d+ -\d+\)$/.test(preview.split("\n")[0]));
  const card = !!approval && keyed;
  const extra = options.filter((item) => !CARD.has(item.value));
  const more = offers(ALWAYS) || offers(GUIDANCE) || extra.length > 0;
  // The rule an approval offers to keep, in words: "run git push *".
  const rule = labelOf(ALWAYS).replace(/^[^:]*: /, "");
  const allow = (answer: string) =>
    void send(remember === "always" && offers(ALWAYS) ? ALWAYS : answer);
  return (
    <section class="card decision enters" aria-label="Pending decision">
      <div class="decision-head">
        <div role="alert">
          <h2 ref={heading} tabIndex={-1}>
            {title}
          </h2>
          {approval && <span class="sr-only">: {approval.action}</span>}
        </div>
        {approval && !asking && (
          <strong class="decision-action truncate">{approval.action}</strong>
        )}
      </div>
      {pending.route === "parent" && (
        <p class="small">You can still answer first.</p>
      )}
      {pending.note && <p class="small">{cleanText(pending.note)}</p>}
      {!asking && (
        <div class="decision-preview">
          {approval && (
            <>
              {diff ? (
                <DiffView text={preview} />
              ) : (
                <pre class="decision-command plain">{preview}</pre>
              )}
              {(cwd || !!approval.risks?.length) && (
                <div class="decision-where">
                  {cwd && (
                    <p class="decision-folder truncate" title={cwd}>
                      in {cwd}
                    </p>
                  )}
                  {!!approval.risks?.length && (
                    <ul class="risk-chips" aria-label="Risks">
                      {approval.risks.map((risk) => (
                        <li
                          class="chip chip-outline risk-chip"
                          data-risk={risk.id}
                          key={risk.id}
                        >
                          {risk.label}
                        </li>
                      ))}
                    </ul>
                  )}
                </div>
              )}
            </>
          )}
          {approval?.reason &&
            (approval.source === "mandatory" ? (
              <p class="small">
                <Notice tone="warning">{cleanText(approval.reason)}</Notice>
              </p>
            ) : (
              <p class="small muted">{cleanText(approval.reason)}</p>
            ))}
          <AttachmentList
            files={(pending.files ?? []).map((file) => ({
              ...file,
              image: file.mime.startsWith("image/"),
            }))}
            href={(id) => routes.asset(session, id)}
          />
          {/* An approval's prompt names the tool; its head says the action. */}
          {!approval && <p>{cleanText(pending.prompt)}</p>}
        </div>
      )}
      {failed && (
        <Notice
          tone="error"
          action={
            <Button
              size="compact"
              disabled={idle}
              onClick={() => void answer(failed.fields)}
            >
              Retry
            </Button>
          }
        >
          {failed.message}
        </Notice>
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
      ) : card ? (
        <form
          class="decision-card"
          onSubmit={(event) => {
            event.preventDefault();
            void send(guidance);
          }}
        >
          {/* The usual answer comes last, nearest the thumb. */}
          <div class="decision-answers">
            {offers(DENY) && (
              <Button
                disabled={idle}
                onClick={() =>
                  void send(
                    remember === "never" && offers(NEVER) ? NEVER : DENY,
                  )
                }
              >
                {labelOf(DENY)}
              </Button>
            )}
            {offers(ALLOW_SESSION) && (
              <Button disabled={idle} onClick={() => allow(ALLOW_SESSION)}>
                {labelOf(ALLOW_SESSION)}
              </Button>
            )}
            {offers(ALLOW_ONCE) && (
              <Button
                variant="primary"
                disabled={idle}
                onClick={() => allow(ALLOW_ONCE)}
              >
                {labelOf(ALLOW_ONCE)}
              </Button>
            )}
          </div>
          {more && (
            <details class="decision-more">
              <summary class="small">More options</summary>
              <div class="decision-more-body">
                {offers(ALWAYS) && (
                  <div class="decision-remember">
                    <p id={`${pending.id}-remember`}>
                      From now on in this folder, {cleanText(rule)}:
                    </p>
                    <div
                      class="segmented"
                      role="group"
                      aria-labelledby={`${pending.id}-remember`}
                    >
                      {(
                        [
                          ["", "Ask each time"],
                          ["always", "Always allow"],
                          ...(offers(NEVER)
                            ? [["never", "Never allow"] as const]
                            : []),
                        ] as const
                      ).map(([value, label]) => (
                        <Button
                          key={value}
                          aria-pressed={remember === value}
                          disabled={idle}
                          onClick={() => setRemember(value)}
                        >
                          {label}
                        </Button>
                      ))}
                    </div>
                    <p class="muted">
                      Kept when you answer. Settings › This folder lists it.
                    </p>
                  </div>
                )}
                {(extra.length > 0 || offers(GUIDANCE)) && (
                  <Actions>
                    {extra.map((item) => (
                      <Button
                        key={item.value}
                        disabled={idle}
                        onClick={() => void send(item.value)}
                      >
                        {item.label}
                      </Button>
                    ))}
                    {offers(GUIDANCE) && (
                      <Button
                        variant="quiet"
                        aria-expanded={reply === GUIDANCE}
                        disabled={idle}
                        onClick={() =>
                          setReply(reply === GUIDANCE ? DENY : GUIDANCE)
                        }
                      >
                        + guidance
                      </Button>
                    )}
                  </Actions>
                )}
                {reply === GUIDANCE && (
                  <InputRow class="decision-guidance">
                    {guidanceInput}
                    <Button type="submit" variant="primary" disabled={idle}>
                      Send guidance
                    </Button>
                  </InputRow>
                )}
              </div>
            </details>
          )}
        </form>
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
                variant={item.value === ALLOW_ONCE ? "primary" : "secondary"}
                aria-pressed={
                  item.value === GUIDANCE ? reply === GUIDANCE : undefined
                }
                disabled={idle}
                onClick={() =>
                  item.value === GUIDANCE
                    ? setReply(GUIDANCE)
                    : void send(item.value)
                }
              >
                {item.label}
              </Button>
            ))}
          </Actions>
          {reply === GUIDANCE && (
            <>
              {guidanceInput}
              <Button type="submit" variant="primary" disabled={idle}>
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
            <fieldset class="decision-options">
              <legend>Response</legend>
              <div class="ask-options">
                {options.map((item) => (
                  <label class="card ask-option" key={item.value}>
                    <Input
                      type="radio"
                      name="reply"
                      value={item.value}
                      checked={reply === item.value}
                      onChange={() => setReply(item.value)}
                    />
                    <span>
                      <strong>{cleanText(item.label)}</strong>
                    </span>
                  </label>
                ))}
              </div>
            </fieldset>
          ) : (
            <label>
              Response
              <Input
                aria-label="Response"
                name="reply"
                type={pending.kind === "secret" ? "password" : undefined}
                autoComplete="off"
                value={reply}
                onInput={(event) => setReply(event.currentTarget.value)}
              />
            </label>
          )}
          <Actions>
            <Button onClick={cancel} disabled={idle}>
              Cancel
            </Button>
            <Button type="submit" variant="primary" disabled={idle || !reply}>
              Send response
            </Button>
          </Actions>
        </form>
      )}
    </section>
  );
}
