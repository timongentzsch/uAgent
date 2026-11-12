import { useState } from "preact/hooks";
import {
  Actions,
  Button,
  EmptyState,
  Field,
  Group,
  Input,
  Modal,
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

// The rules of the active cell's column as the page says them, each with
// Edit and Remove, and a form for one. A change is told to the page by the
// rule's place among them; the page then says the rules as the file has them.
export function RulesDialog({
  cell,
  act,
  close,
}: {
  cell: Cell;
  act: Act;
  close: () => void;
}) {
  const rules = cell.rules ?? [];
  const [draft, setDraft] = useState<Draft>();
  const change = (part: Partial<Draft>) => setDraft({ ...draft!, ...part });
  // Without a rule the one at that place is taken out.
  const set = (place: number, rule?: Pick<Rule, "text" | "row">) =>
    act("rule", JSON.stringify({ place, rule }));
  return (
    <Modal
      title={`Conditional formatting: column ${cell.ref.replace(/^.*!|\d+$/g, "")}`}
      close={close}
    >
      {draft ? (
        <form
          onSubmit={(event) => {
            event.preventDefault();
            set(draft.at, { text: textOf(draft), row: draft.row });
            setDraft(undefined);
          }}
        >
          <Field label="When the cell">
            <Select
              value={draft.sign}
              onChange={(event) => change({ sign: event.currentTarget.value })}
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
                autoFocus
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
          <Actions>
            <Button onClick={() => setDraft(undefined)}>Cancel</Button>
            <Button type="submit" variant="primary" disabled={!valid(draft)}>
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
                onClick={() =>
                  setDraft({
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
                      onClick={() => setDraft(read)}
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
          <Actions>
            <Button onClick={close}>Done</Button>
          </Actions>
        </>
      )}
    </Modal>
  );
}
