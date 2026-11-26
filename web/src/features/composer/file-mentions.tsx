import type { RefObject, TargetedKeyboardEvent } from "preact";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { fileQuote } from "../../shared/file-types.ts";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { bytes } from "../../shared/quantities.ts";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import type { Asset, Draft } from "../../shared/types.ts";
import { useFileSearch } from "../../state/use-file-search.ts";
import {
  encodeMention,
  matchMention,
  mentionOptions,
  workspaceMention,
} from "./mention.ts";

type FileOption = Asset | string;
const keyOf = (item: FileOption) =>
  typeof item === "string" ? `path:${item}` : `asset:${item.id}`;

export function useFileMentions(
  session: string,
  draft: Draft,
  change: (draft: Draft) => void,
  input: RefObject<HTMLTextAreaElement | null>,
  connected: boolean,
  inline: boolean,
) {
  const [caret, at] = useState(0);
  const [selected, select] = useState(-1);
  const [closed, close] = useState(true);
  // Where the caret goes once the picked text is in the field: in the same
  // commit, so nothing typed after the pick lands before it moves.
  const caretTo = useRef<number | null>(null);
  useLayoutEffect(() => {
    const position = caretTo.current;
    if (position === null) return;
    caretTo.current = null;
    input.current?.focus({ preventScroll: true });
    input.current?.setSelectionRange(position, position);
  });
  const mention = closed ? null : matchMention(draft.text, caret);
  const { path, lines } = workspaceMention(mention?.query ?? "");
  const search = useFileSearch(
    session,
    connected && mention ? path : undefined,
  );
  const options: FileOption[] = mention
    ? [
        ...mentionOptions(
          draft.files.filter((file) => !file.pending),
          mention.query,
        ),
        ...(search.value?.paths.slice(0, 12) ?? []),
      ]
    : [];
  const active = options[selected];

  function pick(item: FileOption) {
    if (!mention) return;
    const before = draft.text.slice(0, mention.start);
    const after = draft.text.slice(mention.end);
    let token =
      typeof item === "string"
        ? fileQuote(item, lines)
        : encodeMention(item.name, item.id);
    let quotes = draft.quotes;
    // Side questions and slash commands only send their own text.
    if (typeof item === "string" && !inline && !draft.text.startsWith("/")) {
      if (!quotes.includes(token)) quotes = [...quotes, token];
      token = "";
    }
    const insertion = token && token + (/^\s/.test(after) ? "" : " ");
    const tail =
      !token && before.endsWith(" ") && after.startsWith(" ")
        ? after.slice(1)
        : after;
    change({
      ...draft,
      text: before + insertion + tail,
      quotes,
    });
    close(true);
    select(-1);
    caretTo.current = before.length + insertion.length;
  }
  function keyDown(event: TargetedKeyboardEvent<HTMLTextAreaElement>) {
    if (!mention || !plainKey(event) || event.keyCode === 229) return false;
    if (event.key === "Escape") close(true);
    else if (!options.length) return false;
    else if (event.key === "ArrowDown" || event.key === "ArrowUp")
      select(nextIndex(selected, event.key, options.length));
    else if (event.key === "Tab" || (event.key === "Enter" && active)) {
      const target = active ?? (options.length === 1 ? options[0] : undefined);
      if (!target) return false;
      if (!event.repeat) pick(target);
    } else return false;
    event.preventDefault();
    return true;
  }

  return {
    at,
    changed(position: number) {
      at(position);
      select(-1);
      close(false);
    },
    blur: () => close(true),
    keyDown,
    count: options.length,
    attributes: options.length
      ? {
          "aria-expanded": true,
          "aria-controls": "mention-suggestions",
          "aria-activedescendant": active ? `mention-${selected}` : undefined,
        }
      : {},
    list: !!options.length && (
      <SuggestionList
        id="mention-suggestions"
        label="File mentions"
        prefix="mention"
        items={options}
        index={selected}
        pick={pick}
        keyOf={keyOf}
        groupOf={(item) =>
          typeof item === "string" ? "Workspace references" : "Attachments"
        }
      >
        {(item) =>
          typeof item === "string" ? (
            <span class="row-label" title={item}>
              {item}
              {lines && `:${lines}`}
            </span>
          ) : (
            <>
              <strong>@{item.name}</strong>
              <small>{bytes(item.bytes)}</small>
            </>
          )
        }
      </SuggestionList>
    ),
  };
}
