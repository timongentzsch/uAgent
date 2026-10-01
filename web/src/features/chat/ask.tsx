import "../composer/attachments.css";
import "./ask.css";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { Check, ImagePlus, X } from "lucide-preact";
import type {
  Asset,
  AskAnswer,
  AskQuestion,
  Report,
} from "../../shared/types.ts";
import {
  Actions,
  Button,
  Group,
  IconButton,
  Input,
  Row,
  cleanText,
} from "../../shared/ui.tsx";
import { motionMs } from "../../shared/motion.ts";
import { ImageTile } from "../../shared/attachments.tsx";
import { uploadAttachment } from "../../state/api.ts";
import { bytes } from "../../shared/quantities.ts";
import { maxUploadBytes } from "../../shared/limits.ts";

interface Answer {
  choices: string[];
  // "Other" is chosen; its text counts only then.
  other: boolean;
  text: string;
  image?: Asset;
  uploading?: boolean;
}

const blank = (): Answer => ({ choices: [], other: false, text: "" });
const answered = (answer: Answer) =>
  answer.choices.length > 0 || (answer.other && !!answer.text.trim());

// The model's questions, one page each: the options, a free-text Other and
// an optional image. Steps above the page show where you are and lead back;
// three or more questions end on a review. Submit sends every answer at
// once, in question order.
export default function Ask({
  id,
  session,
  questions,
  online,
  sending,
  send,
  cancel,
  report,
}: {
  id: string;
  session: string;
  questions: AskQuestion[];
  online: boolean;
  sending: boolean;
  send: (text: string, attachmentIds: string[]) => void;
  cancel: () => void;
  report: Report;
}) {
  const [answers, setAnswers] = useState(() => questions.map(blank));
  const update = (index: number, change: Partial<Answer>) =>
    setAnswers((current) =>
      current.map((answer, at) =>
        at === index ? { ...answer, ...change } : answer,
      ),
    );
  const attach = async (index: number, file?: File) => {
    if (!file) return;
    if (file.size > maxUploadBytes) {
      report(new Error(`Attach an image of at most ${bytes(maxUploadBytes)}.`));
      return;
    }
    update(index, { uploading: true });
    try {
      const image = await uploadAttachment(session, file, file.name || "image");
      update(index, { image, uploading: false });
    } catch (failure) {
      report(failure);
      update(index, { uploading: false });
    }
  };
  const submit = () => {
    const wire: AskAnswer[] = answers.map((answer) => ({
      choices: answer.choices,
      other: answer.other ? answer.text.trim() : "",
      ...(answer.image ? { attachment_id: answer.image.id } : {}),
    }));
    send(
      JSON.stringify(wire),
      answers.flatMap((answer) => (answer.image ? [answer.image.id] : [])),
    );
  };
  const ready =
    answers.every(answered) && !answers.some((answer) => answer.uploading);
  // The review is a page of its own, after the last question.
  const review = questions.length >= 3;
  const last = questions.length - (review ? 0 : 1);
  const [page, setPage] = useState(0);
  // The furthest page seen: the steps lead back, never past it.
  const [reached, setReached] = useState(0);
  const panel = useRef<HTMLDivElement>(null);
  const advance = useRef<ReturnType<typeof setTimeout>>();
  const go = (to: number) => {
    clearTimeout(advance.current);
    setPage(to);
    setReached((current) => Math.max(current, to));
  };
  // A new page takes focus, so its question is read; the first one leaves
  // focus where the decision put it.
  const shown = useRef(page);
  useLayoutEffect(() => {
    if (shown.current === page) return;
    shown.current = page;
    panel.current?.focus({ preventScroll: true });
  }, [page]);
  useEffect(() => () => clearTimeout(advance.current), []);
  const index = page;
  const item = questions[index];
  const complete = (at: number) =>
    answered(answers[at]) && !answers[at].uploading;
  const steps = [
    ...questions.map((each) => cleanText(each.header)),
    ...(review ? ["Review"] : []),
  ];
  return (
    <form
      class="ask"
      onSubmit={(event) => {
        event.preventDefault();
        if (page < last) {
          if (complete(page)) go(page + 1);
        } else if (ready) submit();
      }}
    >
      {steps.length > 1 && (
        <div class="ask-steps">
          <div
            class="tabs"
            role="tablist"
            aria-label="Questions"
            onKeyDown={(event) => {
              const by = { ArrowLeft: -1, ArrowRight: 1 }[event.key];
              const to = page + (by ?? 0);
              if (!by || to < 0 || to > reached) return;
              event.preventDefault();
              go(to);
              document.getElementById(`${id}-step-${to}`)?.focus();
            }}
          >
            {steps.map((step, at) => (
              <Button
                size="compact"
                id={`${id}-step-${at}`}
                role="tab"
                title={step}
                aria-controls={`${id}-page`}
                aria-selected={at === page}
                tabIndex={at === page ? 0 : -1}
                disabled={at > reached}
                key={at}
                onClick={() => go(at)}
              >
                {at < questions.length && complete(at) && (
                  <Check aria-label="Answered" />
                )}
                {step}
              </Button>
            ))}
          </div>
          <small class="muted">
            {page + 1} / {steps.length}
          </small>
        </div>
      )}
      <div
        class="ask-questions"
        ref={panel}
        id={`${id}-page`}
        tabIndex={-1}
        role={steps.length > 1 ? "tabpanel" : undefined}
        aria-labelledby={steps.length > 1 ? `${id}-step-${page}` : undefined}
      >
        {(() => {
          if (!item)
            return (
              <div class="ask-page" key={page}>
                <Group title="Your answers">
                  {questions.map((each, at) => (
                    <Row
                      key={at}
                      label={cleanText(each.header)}
                      detail={
                        [
                          ...answers[at].choices,
                          answers[at].other && answers[at].text.trim(),
                          answers[at].image?.name,
                        ]
                          .filter(Boolean)
                          .map((part) => cleanText(String(part)))
                          .join(", ") || "Not answered"
                      }
                      onClick={() => go(at)}
                    />
                  ))}
                </Group>
              </div>
            );
          const answer = answers[index];
          const multiple = !!item.multi_select;
          const type = multiple ? "checkbox" : "radio";
          const name = `${id}-${index}`;
          const heading = cleanText(item.header);
          // Options the agent showed read as cards wide enough to judge.
          const visual = item.options.some(
            (option) => option.image?.id || option.preview,
          );
          return (
            <div class="ask-page" key={page}>
              <fieldset class="ask-question">
                <legend>
                  {/* Its step names it; alone, the chip does. */}
                  {steps.length === 1 && (
                    <span class="ask-chip">{heading}</span>
                  )}
                  {cleanText(item.question)}
                  {multiple && <small class="muted"> Choose any.</small>}
                </legend>
                <div class={`ask-options${visual ? " visual" : ""}`}>
                  {item.options.map((option) => (
                    <label
                      class="ask-option"
                      key={option.label}
                      // One choice answers the question: a tap moves on once
                      // the choice has shown. Arrow keys only move the choice,
                      // and the picture's own button opens it instead.
                      onClick={(event) => {
                        if (
                          multiple ||
                          page >= last ||
                          !event.detail ||
                          (event.target as Element).closest("button, a")
                        )
                          return;
                        clearTimeout(advance.current);
                        advance.current = setTimeout(
                          () => go(index + 1),
                          motionMs("base"),
                        );
                      }}
                    >
                      <Input
                        type={type}
                        name={name}
                        checked={answer.choices.includes(option.label)}
                        onChange={(event) => {
                          const on = event.currentTarget.checked;
                          update(
                            index,
                            multiple
                              ? {
                                  // In the options' order, not the clicks'.
                                  choices: item.options
                                    .map((each) => each.label)
                                    .filter((label) =>
                                      label === option.label
                                        ? on
                                        : answer.choices.includes(label),
                                    ),
                                }
                              : { choices: [option.label], other: false },
                          );
                        }}
                      />
                      <span>
                        <strong>{cleanText(option.label)}</strong>
                        {option.description && (
                          <small>{cleanText(option.description)}</small>
                        )}
                      </span>
                      {option.image?.id && (
                        <span class="ask-shot">
                          <ImageTile
                            name={cleanText(option.description || option.label)}
                            src={`/api/sessions/${session}/assets/${option.image.id}`}
                          />
                        </span>
                      )}
                      {option.preview && (
                        <pre class="ask-preview">
                          {cleanText(option.preview)}
                        </pre>
                      )}
                    </label>
                  ))}
                  <label class="ask-option">
                    <Input
                      type={type}
                      name={name}
                      checked={answer.other}
                      onChange={(event) =>
                        update(
                          index,
                          multiple
                            ? { other: event.currentTarget.checked }
                            : { other: true, choices: [] },
                        )
                      }
                    />
                    <span>
                      <strong>Other</strong>
                      <small>Answer in your own words.</small>
                    </span>
                  </label>
                </div>
                {answer.other && (
                  <Input
                    aria-label={`Other answer: ${heading}`}
                    placeholder="Your answer"
                    autoComplete="off"
                    value={answer.text}
                    onInput={(event) =>
                      update(index, { text: event.currentTarget.value })
                    }
                  />
                )}
                {answer.image ? (
                  <div class="file-chip">
                    {answer.image.image && (
                      <ImageTile
                        name={answer.image.name}
                        src={`/api/sessions/${session}/assets/${answer.image.id}`}
                      />
                    )}
                    <span title={answer.image.name}>
                      {answer.image.name}
                      <small>{bytes(answer.image.bytes)}</small>
                    </span>
                    <IconButton
                      label={`Remove ${answer.image.name}`}
                      onClick={() => update(index, { image: undefined })}
                    >
                      <X />
                    </IconButton>
                  </div>
                ) : (
                  <label
                    class="file-button quiet"
                    aria-busy={answer.uploading || undefined}
                  >
                    <ImagePlus />
                    {answer.uploading ? "Uploading…" : "Attach image"}
                    <Input
                      type="file"
                      accept="image/*"
                      aria-label={`Attach image: ${heading}`}
                      disabled={!online || answer.uploading}
                      onChange={(event) => {
                        void attach(index, event.currentTarget.files?.[0]);
                        event.currentTarget.value = "";
                      }}
                    />
                  </label>
                )}
              </fieldset>
            </div>
          );
        })()}
      </div>
      <Actions>
        <Button onClick={cancel} disabled={!online || sending}>
          Cancel
        </Button>
        {page > 0 && <Button onClick={() => go(page - 1)}>Back</Button>}
        {/* Keyed: Next never becomes Submit under a second click. */}
        {page < last ? (
          <Button
            key="next"
            type="submit"
            variant="primary"
            disabled={!complete(page)}
          >
            Next
          </Button>
        ) : (
          <Button
            key="submit"
            type="submit"
            variant="primary"
            disabled={!online || sending || !ready}
          >
            Submit
          </Button>
        )}
      </Actions>
    </form>
  );
}
