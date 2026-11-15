import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import {
  Actions,
  Button,
  EmptyState,
  Field,
  Group,
  Input,
  Modal,
  Notice,
  Row,
  Select,
  Switch,
  ValueSelect,
} from "../../shared/ui.tsx";
import { capitalize } from "../../shared/display.ts";
import type { Rule } from "../../shared/page-frame.tsx";
import type { Cell } from "./sheet-edit.ts";
import { type Act, COLOURS } from "./sheet.tsx";

// A column's conditional formatting, set by a person in the words the
// agent writes it in (skills/artifact/references/sheet.md): each rule is
// `condition: effects`, on the column's cells or on their rows.

const CONDITIONS = [
  ["Is", "="],
  ["Is not", "<>"],
  ["Is less than", "<"],
  ["Is at most", "<="],
  ["Is greater than", ">"],
  ["Is at least", ">="],
  ["Is blank", "blank"],
  ["Is not blank", "not blank"],
  ["Is before today", "before today"],
  ["Is after today", "after today"],
];
// The conditions that take a value; more or less is of an amount.
const COMPARES = /^(<>|<=|>=|=|<|>)\s*/;
const AMOUNT = /^(-?\d+(\.\d+)?%?|\d{4}-\d\d-\d\d)$/;

// A rule as its form holds it, at its place among the column's.
interface Draft {
  at: number;
  row: boolean;
  sign: string;
  value: string;
  fill: string;
  bold: boolean;
  strike: boolean;
}
// Nothing for a rule the page did not understand.
const draftOf = ({ row, condition, effects }: Rule, at: number) => {
  if (condition === undefined || !effects) return undefined;
  const [whole, sign] = COMPARES.exec(condition) ?? ["", condition];
  return {
    at,
    row,
    sign,
    value: whole && condition.slice(whole.length),
    fill:
      effects.findLast((word) => COLOURS.some(([, fill]) => fill === word)) ??
      "",
    bold: effects.includes("bold"),
    strike: effects.includes("strike"),
  };
};
const amount = ({ sign }: Draft) => sign !== "=" && sign !== "<>";
const valid = (draft: Draft) => {
  const value = draft.value.trim();
  return (
    !!(draft.fill || draft.bold || draft.strike) &&
    (!COMPARES.test(draft.sign) ||
      (amount(draft) ? AMOUNT.test(value) : !!value && !value.includes(";")))
  );
};
const textOf = ({ sign, value, fill, bold, strike }: Draft) =>
  `${sign}${COMPARES.test(sign) ? ` ${value.trim()}` : ""}: ${[
    fill,
    bold && "bold",
    strike && "strike",
  ]
    .filter(Boolean)
    .join(" ")}`;
const wordsOf = ({ sign, value }: Draft) =>
  `${CONDITIONS.find(([, said]) => said === sign)?.[0]} ${value}`.trim();
const looksOf = ({ fill, bold, strike, row }: Draft) =>
  `${capitalize(
    [
      fill &&
        `${COLOURS.find(([, said]) => said === fill)?.[0].toLowerCase()} fill`,
      bold && "bold",
      strike && "strike",
    ]
      .filter(Boolean)
      .join(", "),
  )} on the ${row ? "whole row" : "cell"}`;

// The column a cell is in, with its tab: the one a dialog is opened for.
export const columnOf = (cell: Cell) => cell.ref.replace(/\d+$/, "");

// The rules of the active cell's column as the page says them, each with
// Edit and Remove, and a form for one. A change is told to the page by the
// rule's place among them and what stands there; the page then says the
// cell again, with the rules as the file has them, or refuses.
export function RulesDialog({
  cell,
  refusal,
  act,
  close,
}: {
  cell: Cell;
  // Why the page last refused a change.
  refusal?: { text: string };
  act: Act;
  close: () => void;
}) {
  const rules = cell.rules ?? [];
  const [draft, setDraft] = useState<Draft>();
  const change = (part: Partial<Draft>) => setDraft({ ...draft!, ...part });
  // A form sent stays until the page answers: with the cell, which closes
  // it, or with a refusal, which is said in it.
  const [sent, setSent] = useState(false);
  const [problem, setProblem] = useState<string>();
  const heard = useRef(refusal);
  useEffect(() => {
    if (!refusal || refusal === heard.current) return;
    heard.current = refusal;
    setProblem(refusal.text);
    setSent(false);
  }, [refusal]);
  // biome-ignore lint/correctness/useExhaustiveDependencies: the page's answer is the cell said again
  useEffect(() => {
    if (!sent) return;
    setSent(false);
    setDraft(undefined);
  }, [cell]);
  // Without a rule the one at that place is taken out.
  const set = (place: number, rule?: Pick<Rule, "text" | "row">) => {
    setProblem(undefined);
    act(
      "rule",
      JSON.stringify({
        of: columnOf(cell),
        place,
        text: rules[place]?.text,
        rule,
      }),
    );
  };
  // The keys follow the view: into the form, and back to what opened it.
  const views = useRef<HTMLDivElement>(null);
  const opened = useRef<number>(undefined);
  const formed = !!draft;
  useLayoutEffect(() => {
    const find = (what: string) =>
      views.current?.querySelector<HTMLElement>(what);
    if (opened.current === undefined) return;
    (formed
      ? find("select")
      : (find(`[data-place="${opened.current}"]`) ?? find("[data-place]"))
    )?.focus();
  }, [formed]);
  const form = (from: Draft) => {
    opened.current = from.at;
    setProblem(undefined);
    setDraft(from);
  };
  const said = problem && <Notice tone="error">{problem}</Notice>;
  return (
    <Modal
      title={`Conditional formatting: column ${columnOf(cell).replace(/^.*!/, "")}`}
      close={close}
    >
      <div ref={views}>
        {draft ? (
          <form
            onSubmit={(event) => {
              event.preventDefault();
              set(draft.at, { text: textOf(draft), row: draft.row });
              setSent(true);
            }}
          >
            <Field label="When the cell">
              <Select
                value={draft.sign}
                onChange={(event) =>
                  change({ sign: event.currentTarget.value })
                }
              >
                {CONDITIONS.map(([label, sign]) => (
                  <option key={sign} value={sign}>
                    {label}
                  </option>
                ))}
              </Select>
            </Field>
            {COMPARES.test(draft.sign) && (
              <Field
                label="Value"
                help={`${amount(draft) ? "A number" : "Text without a semicolon, a number"}, a percentage or a date as 2027-03-01`}
              >
                <Input
                  value={draft.value}
                  onInput={(event) =>
                    change({ value: event.currentTarget.value })
                  }
                />
              </Field>
            )}
            <Group title="Format">
              <Row label="Fill">
                <ValueSelect
                  aria-label="Fill"
                  value={draft.fill}
                  onChange={(event) =>
                    change({ fill: event.currentTarget.value })
                  }
                >
                  {[["None", ""], ...COLOURS].map(([label, fill]) => (
                    <option key={fill} value={fill}>
                      {label}
                    </option>
                  ))}
                </ValueSelect>
              </Row>
              <Row label="Bold">
                <Switch
                  label="Bold"
                  checked={draft.bold}
                  onChange={(bold) => change({ bold })}
                />
              </Row>
              <Row label="Strike">
                <Switch
                  label="Strike"
                  checked={draft.strike}
                  onChange={(strike) => change({ strike })}
                />
              </Row>
              <Row label="The whole row" detail="Not only this column's cell">
                <Switch
                  label="The whole row"
                  checked={draft.row}
                  onChange={(row) => change({ row })}
                />
              </Row>
            </Group>
            {said}
            <Actions>
              <Button onClick={() => setDraft(undefined)}>Cancel</Button>
              <Button
                type="submit"
                variant="primary"
                busy={sent}
                disabled={!valid(draft)}
              >
                Save
              </Button>
            </Actions>
          </form>
        ) : (
          <>
            <Group
              title="Rules"
              action={
                <Button
                  size="compact"
                  data-place={rules.length}
                  onClick={() =>
                    form({
                      at: rules.length,
                      row: false,
                      sign: "=",
                      value: "",
                      fill: "",
                      bold: false,
                      strike: false,
                    })
                  }
                >
                  Add rule
                </Button>
              }
            >
              {rules.map((rule, at) => {
                const read = draftOf(rule, at);
                const name = read ? wordsOf(read) : rule.text;
                return (
                  <Row
                    key={`${at} ${rule.text}`}
                    label={name}
                    detail={
                      read
                        ? looksOf(read)
                        : `Not understood, on the ${rule.row ? "whole row" : "cell"}`
                    }
                  >
                    {read && (
                      <Button
                        size="compact"
                        aria-label={`Edit ${name}`}
                        data-place={at}
                        onClick={() => form(read)}
                      >
                        Edit
                      </Button>
                    )}
                    <Button
                      size="compact"
                      aria-label={`Remove ${name}`}
                      onClick={() => set(at)}
                    >
                      Remove
                    </Button>
                  </Row>
                );
              })}
              {!rules.length && (
                <EmptyState>No rule formats this column.</EmptyState>
              )}
            </Group>
            {said}
            <Actions>
              <Button onClick={close}>Done</Button>
            </Actions>
          </>
        )}
      </div>
    </Modal>
  );
}
