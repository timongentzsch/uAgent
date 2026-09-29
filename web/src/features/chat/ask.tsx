import "../composer/attachments.css";
import "./ask.css";
import { useState } from "preact/hooks";
import { ImagePlus, X } from "lucide-preact";
import type {
  Asset,
  AskAnswer,
  AskQuestion,
  Report,
} from "../../shared/types.ts";
import {
  Actions,
  Button,
  IconButton,
  Input,
  cleanText,
} from "../../shared/ui.tsx";
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

// The model's questions, one card each: the options, a free-text Other and
// an optional image. Submit sends every answer at once, in question order.
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
  return (
    <form
      class="ask"
      onSubmit={(event) => {
        event.preventDefault();
        if (ready) submit();
      }}
    >
      <div class="ask-questions">
        {questions.map((item, index) => {
          const answer = answers[index];
          const multiple = !!item.multi_select;
          const type = multiple ? "checkbox" : "radio";
          const name = `${id}-${index}`;
          const heading = cleanText(item.header);
          return (
            <fieldset class="ask-question" key={index}>
              <legend>
                <span class="ask-chip">{heading}</span>
                {cleanText(item.question)}
                {multiple && <small class="muted"> Choose any.</small>}
              </legend>
              <div class="ask-options">
                {item.options.map((option) => (
                  <label class="ask-option" key={option.label}>
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
          );
        })}
      </div>
      <Actions>
        <Button onClick={cancel} disabled={!online || sending}>
          Cancel
        </Button>
        <Button
          type="submit"
          variant="primary"
          disabled={!online || sending || !ready}
        >
          Submit
        </Button>
      </Actions>
    </form>
  );
}
