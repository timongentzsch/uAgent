import {
  useEffect,
  useId,
  useLayoutEffect,
  useRef,
  useState,
} from "preact/hooks";
import type { Session } from "./types.ts";
import { Avatar } from "./avatar.tsx";
import { Menu, MenuItem } from "./menu.tsx";
import { SheetButton } from "./sheet.tsx";
import { Button } from "./ui.tsx";
import { coarsePointer, placePopover } from "./layout.ts";
import { useHostContext } from "../state/host-context.ts";
import "./member-card.css";

// How long the pointer rests on an avatar before its card opens, and how
// long it may stray between the avatar and the card before it closes.
const OPEN_MS = 250;
const CLOSE_MS = 200;

const MODERATOR_ROLE = "Hands work to threads; never edits files.";

// Who a member is, with what to do with it: ask it, read its prompt,
// rename or remove it. The moderator is the panel's own session.
function MemberCard({ item, done }: { item: Session; done: () => void }) {
  const { online, setModal, showContext, askMember } = useHostContext();
  const name = item.member || "";
  const moderator = item.kind === "panel";
  return (
    <div class="member-card">
      <header>
        <Avatar name={name} />
        <div>
          <b>{name}</b>
          <span>
            {moderator ? "Moderator" : "Member"}
            {item.model && ` · ${item.model}`}
          </span>
        </div>
      </header>
      <section>
        <h3>{moderator ? "ROLE" : "PERSONA"}</h3>
        <p>{moderator ? MODERATOR_ROLE : item.persona}</p>
      </section>
      {item.skills && (
        <section>
          <h3>SKILLS</h3>
          <p>{item.skills}</p>
        </section>
      )}
      <footer>
        <Button
          variant="primary"
          onClick={() => {
            done();
            askMember(item);
          }}
        >
          Ask {name}
        </Button>
        <Button
          onClick={() => {
            done();
            showContext(item);
          }}
        >
          Full prompt
        </Button>
        <Menu label={`Menu for ${name}`}>
          <MenuItem
            disabled={!online}
            onClick={() => {
              done();
              setModal({ type: "rename", session: item });
            }}
          >
            Rename
          </MenuItem>
          {!moderator && (
            <MenuItem
              disabled={!online}
              onClick={() => {
                done();
                setModal({ type: "delete", session: item });
              }}
            >
              Remove
            </MenuItem>
          )}
        </Menu>
      </footer>
    </div>
  );
}

// A member's avatar, which answers who it is: on a pointer that hovers, a
// card beside it while it is pointed at or focused; on a touch screen, the
// same in a sheet.
export function MemberAvatar({ item }: { item: Session }) {
  const name = item.member || "";
  const label = `${name}, ${item.kind === "panel" ? "moderator" : "member"}`;
  const [open, setOpen] = useState(false);
  const anchor = useRef<HTMLButtonElement>(null);
  const card = useRef<HTMLDivElement>(null);
  const timer = useRef(0);
  const id = useId();
  const later = (value: boolean, delay: number) => {
    clearTimeout(timer.current);
    timer.current = window.setTimeout(() => setOpen(value), delay);
  };
  useEffect(() => () => clearTimeout(timer.current), []);
  useLayoutEffect(() => {
    const element = card.current;
    const button = anchor.current;
    if (!open || !element || !button) return;
    element.showPopover();
    // Under the avatar, or over it where there is no room below.
    placePopover(element, button, "start");
    const escape = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      if (element.contains(document.activeElement))
        button.focus({ preventScroll: true });
      setOpen(false);
    };
    document.addEventListener("keydown", escape);
    return () => document.removeEventListener("keydown", escape);
  }, [open]);
  if (coarsePointer())
    return (
      <SheetButton
        label={label}
        heading={name}
        trigger={<Avatar name={name} />}
        className="member-avatar"
        buttonClass="avatar-button"
        size="icon"
      >
        {(close) => <MemberCard item={item} done={close} />}
      </SheetButton>
    );
  return (
    <span
      class="member-avatar"
      onMouseEnter={() => later(true, OPEN_MS)}
      onMouseLeave={() => later(false, CLOSE_MS)}
      onFocusIn={() => {
        clearTimeout(timer.current);
        setOpen(true);
      }}
      onFocusOut={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget as Node | null))
          later(false, 0);
      }}
    >
      <Button
        variant="quiet"
        buttonRef={anchor}
        class="avatar-button"
        aria-label={label}
        aria-haspopup="dialog"
        aria-expanded={open}
        aria-controls={open ? id : undefined}
        onClick={() => {
          clearTimeout(timer.current);
          setOpen(true);
        }}
      >
        <Avatar name={name} />
      </Button>
      {open && (
        <div
          ref={card}
          id={id}
          popover="manual"
          role="dialog"
          aria-label={name}
          class="member-popover"
        >
          <MemberCard item={item} done={() => setOpen(false)} />
        </div>
      )}
    </span>
  );
}
