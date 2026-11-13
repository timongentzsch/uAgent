import { useState } from "preact/hooks";
import type { PlaceOption, Session, Space } from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import { useHostContext } from "../../state/host-context.ts";
import { folderOf } from "../../shared/display.ts";
import { filingTag, nameOf, optionsOf, unfiled } from "../../shared/filing.ts";
import { useAction } from "../../shared/use-action.ts";
import SessionName from "../../shared/session-name.tsx";
import { Button, DataText, SectionTitle } from "../../shared/ui.tsx";
import "./filing.css";

// Where a first message goes instead of the conversation it started.
export type Redirect = (
  from: Session,
  text: string,
  to: { cwd: string; session?: Session },
) => Promise<unknown>;

// The host's suggestion for one conversation in Scratch, with its answers.
// `first` is its only message while its first turn has changed no file: it
// can then still start over in the place, or join the conversation it
// continues.
export function Suggestion({
  session,
  sessions,
  spaces,
  first = "",
  redirect,
  compact,
}: {
  session: Session;
  sessions: Session[];
  spaces: Space[];
  first?: string;
  redirect?: Redirect;
  compact?: boolean;
}) {
  const { command } = useApi();
  const { online, setModal, report } = useHostContext();
  const { run, busy } = useAction({ report });
  const [answered, setAnswered] = useState(false);
  const options = optionsOf(session);
  if (answered || !options.length) return null;
  const early = !!first && !!redirect;
  const joined = sessions.find((item) => item.id === options[0].session);
  const names = options.map((option) => nameOf(option, spaces));
  const blocked = !online || busy || (!early && !!session.turn_active);
  const act = (work: () => Promise<unknown>) =>
    void run(async () => {
      await work();
      setAnswered(true);
    });
  const move = (option: PlaceOption) =>
    act(() =>
      command(
        "move",
        session,
        option.workspace ? { space: option.workspace } : { cwd: option.cwd },
      ),
    );
  const title =
    early && joined ? (
      <>
        Continue “<SessionName item={joined} />” in {names[0]}?
      </>
    ) : early ? (
      `Run this in ${names[0]} instead?`
    ) : options[0].workspace ? (
      `A new workspace, “${names[0]}”`
    ) : options.length > 1 ? (
      `This fits ${names.join(" or ")}`
    ) : (
      `This belongs in ${names[0]}`
    );
  const buttons = early ? (
    <>
      {joined && (
        <Button
          variant="primary"
          disabled={blocked}
          onClick={() =>
            act(() =>
              redirect(session, first, {
                cwd: folderOf(joined),
                session: joined,
              }),
            )
          }
        >
          Continue there
        </Button>
      )}
      {options[0].cwd && (
        <Button
          variant={joined ? "secondary" : "primary"}
          disabled={blocked}
          onClick={() =>
            act(() => redirect(session, first, { cwd: options[0].cwd || "" }))
          }
        >
          New in {names[0]}
        </Button>
      )}
    </>
  ) : (
    options.map((option, index) => (
      <Button
        key={names[index]}
        variant={options.length === 1 ? "primary" : "secondary"}
        disabled={blocked}
        onClick={() => move(option)}
      >
        {option.workspace ? "Create and move" : `Move to ${names[index]}`}
      </Button>
    ))
  );
  if (compact) return <span class="filing-actions">{buttons}</span>;
  return (
    <section class="filing card" aria-label="Suggested place">
      <SectionTitle>Suggested place</SectionTitle>
      <p>
        <DataText>{title}</DataText>
      </p>
      <p class="small muted">
        <DataText>
          {[
            session.suggestion?.reason,
            early && "Nothing has changed yet, so it starts over there.",
            !early && session.turn_active && "It can move when this turn ends.",
          ]
            .filter(Boolean)
            .join(" ")}
        </DataText>
      </p>
      <div class="filing-actions">
        {buttons}
        {!early && (
          <Button
            disabled={blocked}
            onClick={() => setModal({ type: "move", session })}
          >
            Other place…
          </Button>
        )}
        <Button
          variant="quiet"
          disabled={!online || busy}
          onClick={() => act(() => command("keep", session))}
        >
          {early ? "Stay in Scratch" : "Keep in Scratch"}
        </Button>
      </div>
    </section>
  );
}

// Home's list of what waits in Scratch.
export function ToFile({
  sessions,
  spaces,
  choose,
}: {
  sessions: Session[];
  spaces: Space[];
  choose: (id: string) => void;
}) {
  const waiting = unfiled(sessions, spaces);
  if (!waiting.length) return null;
  return (
    <section class="to-file" aria-label="To file">
      <SectionTitle>To file · {waiting.length} in Scratch</SectionTitle>
      <ul class="divided card">
        {waiting.map((item) => (
          <li key={item.id}>
            <Button variant="quiet" onClick={() => choose(item.id)}>
              <span class="truncate">
                <SessionName item={item} />
              </span>
              <small class="truncate">
                <DataText>
                  {item.turn_active
                    ? "Working"
                    : [filingTag(item, spaces), item.suggestion?.reason]
                        .filter(Boolean)
                        .join(" · ") || "No place suggested"}
                </DataText>
              </small>
            </Button>
            <Suggestion
              session={item}
              sessions={sessions}
              spaces={spaces}
              compact
            />
          </li>
        ))}
      </ul>
    </section>
  );
}
