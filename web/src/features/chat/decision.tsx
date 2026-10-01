import type {
  Pending,
  Act,
  CommandFields,
  Report,
} from "../../shared/types.ts";
import { failure } from "../../shared/types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import {
  Actions,
  Button,
  cleanText,
  Input,
  Textarea,
} from "../../shared/ui.tsx";
import Ask from "./ask.tsx";
import DiffView from "./diff-view.tsx";

// The approval's own answers (the host's choice table); anything else it
// offers shows as a plain button after them.
const ALLOW_ONCE = "y",
  ALLOW_SESSION = "s",
  ALWAYS = "a",
  DENY = "n",
  GUIDANCE = "guidance";
const CARD = new Set([ALLOW_ONCE, ALLOW_SESSION, ALWAYS, DENY, GUIDANCE]);

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
  const options = (pending.options ?? []).map((item) =>
    typeof item === "string"
      ? { value: item, label: item }
      : { value: item.value, label: item.label || item.title || item.value },
  );
  const offers = (value: string) =>
    options.some((item) => item.value === value);
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
  const [always, setAlways] = useState(false);
  const [sending, setSending] = useState(false);
  // A reply that did not reach the host stays here, with Retry.
  const [failed, setFailed] = useState<{
    message: string;
    fields: CommandFields;
  } | null>(null);
  const approval = pending.approval;
  const asking = pending.kind === "ask";
  // One reply in flight at a time: answer and cancel share the guard.
  const answer = async (fields: CommandFields) => {
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
  // Each new decision (this panel is keyed by it) takes focus, so a
  // keyboard lands on what the agent waits for; only someone typing
  // elsewhere keeps their place. The composer's input, held read-only
  // below a pending decision, is not a place being typed in.
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
    pending.route === "coordinator"
      ? "The coordinator is deciding"
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
  // Anything past the three answers folds under More options.
  const extra = options.filter((item) => !CARD.has(item.value));
  const more = offers(ALWAYS) || offers(GUIDANCE) || extra.length > 0;
  return (
    <section class="decision" aria-label="Pending decision">
      <div class="decision-head">
        {/* Announced at once, with what it is about. */}
        <div role="alert">
          <h2 ref={heading} tabIndex={-1}>
            {title}
          </h2>
          {approval?.tool && <span class="sr-only">: {approval.tool}</span>}
        </div>
        {approval && !asking && (
          <strong
            class="decision-tool"
            title={approval.mandatory_reason || undefined}
          >
            {approval.tool}
            {approval.mandatory_human
              ? ` · ${approval.mandatory_reason || "explicit approval required"}`
              : ""}
          </strong>
        )}
      </div>
      {pending.route === "coordinator" && (
        <p class="decision-note">You can still answer first.</p>
      )}
      {pending.note && <p class="decision-note">{cleanText(pending.note)}</p>}
      {!asking && (
        <div class="decision-preview">
          {approval && (
            <>
              {diff ? (
                <DiffView text={preview} />
              ) : (
                <pre class="decision-command">{preview}</pre>
              )}
              {/* Where it runs and what it risks share one line. */}
              {(cwd || !!approval.risks?.length) && (
                <div class="decision-where">
                  {cwd && (
                    <p class="decision-folder" title={cwd}>
                      in {cwd}
                    </p>
                  )}
                  {!!approval.risks?.length && (
                    <ul class="risk-chips" aria-label="Risks">
                      {approval.risks.map((risk) => (
                        <li class="risk-chip" data-risk={risk.id} key={risk.id}>
                          {risk.label}
                        </li>
                      ))}
                    </ul>
                  )}
                </div>
              )}
            </>
          )}
          <p>{cleanText(pending.prompt)}</p>
        </div>
      )}
      {failed && (
        <p class="failure" role="alert">
          {failed.message}{" "}
          <Button
            size="compact"
            disabled={idle}
            onClick={() => void answer(failed.fields)}
          >
            Retry
          </Button>
        </p>
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
          {/* The three answers in one row of equal buttons, the usual one
              last, nearest the thumb. */}
          <div class="decision-answers">
            {offers(DENY) && (
              <Button disabled={idle} onClick={() => void send(DENY)}>
                Deny
              </Button>
            )}
            {offers(ALLOW_SESSION) && (
              <Button disabled={idle} onClick={() => void send(ALLOW_SESSION)}>
                Allow for session
              </Button>
            )}
            {offers(ALLOW_ONCE) && (
              <Button
                variant="primary"
                disabled={idle}
                onClick={() => void send(always ? ALWAYS : ALLOW_ONCE)}
              >
                Allow once
              </Button>
            )}
          </div>
          {more && (
            <details class="decision-more">
              <summary>More options</summary>
              <div class="decision-more-body">
                {offers(ALWAYS) && (
                  <label class="decision-always">
                    <Input
                      type="checkbox"
                      checked={always}
                      onChange={(event) =>
                        setAlways(event.currentTarget.checked)
                      }
                    />
                    Always allow this exact action here
                  </label>
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
                  <div class="decision-guidance">
                    {guidanceInput}
                    <Button type="submit" variant="primary" disabled={idle}>
                      Send guidance
                    </Button>
                  </div>
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
                  <label class="ask-option" key={item.value}>
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
