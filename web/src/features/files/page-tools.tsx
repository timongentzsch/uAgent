import type { ComponentChildren } from "preact";
import {
  Check,
  ChevronDown,
  ExternalLink,
  FileOutput,
  Undo2,
  Redo2,
} from "lucide-preact";
import { Menu, MenuItem, MenuSub } from "../../shared/menu.tsx";
import { IconButton } from "../../shared/ui.tsx";

export interface PageAction {
  label: string;
  icon: typeof ExternalLink;
  run: () => unknown;
  disabled?: boolean;
  checked?: boolean;
}

function actionButton({
  label,
  icon: Icon,
  run,
  disabled,
  checked,
}: PageAction) {
  return (
    <IconButton
      key={label}
      label={label}
      disabled={disabled}
      aria-pressed={checked}
      onClick={run}
    >
      <Icon />
    </IconButton>
  );
}

function actionItem({ label, run, disabled, checked }: PageAction) {
  return (
    <MenuItem
      key={label}
      role={checked === undefined ? "menuitem" : "menuitemcheckbox"}
      aria-checked={checked}
      disabled={disabled}
      onClick={run}
    >
      {label}
      {checked && <Check />}
    </MenuItem>
  );
}

// One set of commands for the toolbar and its compact menu. Primary editing
// controls retain their positions as the document's undo history changes.
export default function PageTools({
  tools,
  exports,
  open,
  back,
  undoable,
  foldHistory,
  fit,
  children,
}: {
  tools: (PageAction | false)[];
  exports: (false | undefined | [string, () => void])[];
  open: () => void;
  back?: (redo: boolean) => void;
  undoable?: { undo: boolean; redo: boolean };
  foldHistory: boolean;
  fit?: { disabled: boolean; run: () => void };
  children: ComponentChildren;
}) {
  const alone = { label: "Open alone", icon: ExternalLink, run: open };
  const history: PageAction[] = back
    ? [
        {
          label: "Undo",
          icon: Undo2,
          disabled: !undoable?.undo,
          run: () => back(false),
        },
        {
          label: "Redo",
          icon: Redo2,
          disabled: !undoable?.redo,
          run: () => back(true),
        },
      ]
    : [];
  const exportItems = exports.map(
    (item) =>
      item && (
        <MenuItem key={item[0]} onClick={item[1]}>
          {item[0]}
        </MenuItem>
      ),
  );
  return (
    <>
      {history.length > 0 && (
        <span class={foldHistory ? "page-tools-wide" : "page-history"}>
          {history.map(actionButton)}
        </span>
      )}
      {children}
      {/* raw: a narrow pane folds the tools after Edit into one menu. */}
      <span class="page-tools-wide">
        {tools.map((tool) => tool && actionButton(tool))}
        <Menu
          label="Export"
          trigger={
            <>
              <FileOutput />
              Export
              <ChevronDown />
            </>
          }
        >
          {exportItems}
        </Menu>
        {actionButton(alone)}
      </span>
      <span class="page-tools-folded">
        <Menu label="More tools">
          {foldHistory && history.map(actionItem)}
          {tools.map((tool) => tool && actionItem(tool))}
          <MenuSub label="Export">{exportItems}</MenuSub>
          {actionItem(alone)}
          {fit && (
            <MenuItem disabled={fit.disabled} onClick={fit.run}>
              Fit
            </MenuItem>
          )}
        </Menu>
      </span>
    </>
  );
}
