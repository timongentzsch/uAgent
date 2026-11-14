import { useEffect, useState } from "preact/hooks";
import type { FunctionComponent } from "preact";
import {
  BookOpen,
  ChevronDown,
  Brain,
  FileText,
  LayoutDashboard,
  Palette,
  Plus,
  Presentation,
  ScrollText,
  Table,
} from "lucide-preact";
import { useApi } from "../../state/api.ts";
import type {
  FileTarget,
  LibraryItem,
  LibraryKind,
  LibraryListing,
  LibraryPlace,
} from "../../shared/types.ts";
import {
  Button,
  DataText,
  EmptyState,
  Input,
  LoadError,
  Modal,
  Placeholder,
  Select,
  Time,
} from "../../shared/ui.tsx";
import { usePushDirection } from "../../shared/motion.ts";
import { bytes, plural } from "../../shared/quantities.ts";
import { artifactSize } from "../../shared/artifact.ts";
import { useResource } from "../../shared/use-resource.ts";
import { matches } from "../../shared/fuzzy.ts";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import FolderLabel from "../../shared/folder-label.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { useSelection } from "../../shared/selection.ts";
import { RowCheck, SelectionBar } from "../../shared/selection-bar.tsx";
import { useShortcuts } from "../../shared/shortcuts.ts";
import DeleteMany, { deleteTitle } from "../../shared/delete-many.tsx";
import ImportDialog from "./import.tsx";
import LibraryDocument from "./document.tsx";
import LibraryArtifact from "./artifact.tsx";
import "./management.css";

// The Library: everything kept across every folder and workspace, in one list
// grouped by place. A rail filters it by kind and place; an item opens beside
// the list. Nothing is stored here: each item stays where it lives.

const RAIL: [string, [LibraryKind | "", string][]][] = [
  ["", [["", "Everything"]]],
  [
    "Artifacts",
    [
      ["design", "Designs"],
      ["deck", "Decks"],
      ["doc", "Docs"],
      ["sheet", "Sheets"],
      ["system", "Design systems"],
      ["file", "Shared files"],
    ],
  ],
  [
    "Knowledge",
    [
      ["memory", "Memories"],
      ["skill", "Skills"],
    ],
  ],
];
const ICON: Record<LibraryKind, FunctionComponent> = {
  design: LayoutDashboard,
  deck: Presentation,
  doc: ScrollText,
  sheet: Table,
  system: Palette,
  file: FileText,
  memory: Brain,
  skill: BookOpen,
};
const NAME: Record<LibraryKind, string> = {
  design: "design",
  deck: "deck",
  doc: "doc",
  sheet: "sheet",
  system: "design system",
  file: "shared file",
  memory: "memory",
  skill: "skill",
};
const NAMES: Partial<Record<LibraryKind, string>> = { memory: "memories" };
// What deleting one of a kind removes.
const GOES: Record<LibraryKind, string> = {
  design: "deleted from the project, folder and all",
  deck: "deleted from the project, folder and all",
  system: "deleted from the project, folder and all",
  doc: "deleted from the project",
  sheet: "deleted from the project",
  file: "deleted",
  memory: "forgotten",
  skill: "forgotten; supporting files stay",
};
// An item's id in the selection: its place and key.
const idOf = (item: LibraryItem) => `${item.place ?? ""}\n${item.key}`;
// Every place's own, as a place filter; "*" is every place.
const ALL = "*";
// Rows drawn while the list loads (see <Placeholder>).
const SAMPLE = ["An item name", "A longer item name here", "A name"].map(
  (name, index): LibraryItem => ({
    key: `placeholder-${index}`,
    name,
    path: "",
    scope: "",
    source: "",
    writable: false,
    revision: "",
    kind: "memory",
    place: "",
  }),
);
const detail = (item: LibraryItem) =>
  item.kind === "system"
    ? "tokens.css"
    : item.kind === "doc"
      ? "on paper"
      : item.kind === "file"
        ? bytes(item.bytes ?? 0)
        : item.kind === "design" ||
            item.kind === "deck" ||
            item.kind === "sheet"
          ? artifactSize(item.kind, item.count ?? 0)
          : item.description || item.source;
// The control a memory or skill is managed through.
const control = (kind?: LibraryKind) =>
  kind === "skill" ? ("skills" as const) : ("memory" as const);
type Selection =
  | { key: string; place: string }
  | { new: LibraryKind; where: string };

export default function Library({
  online,
  version,
  initialKind = "",
  openFiles,
  choose,
}: {
  online: boolean;
  version: number;
  initialKind?: LibraryKind | "";
  // Shows a path in the Files pane of a conversation in the place.
  openFiles: (place: LibraryPlace, target: FileTarget) => void;
  choose: (session: string) => void;
}) {
  const { api, routes, manage } = useApi();
  const listing = useResource(
    (signal) =>
      online
        ? api<LibraryListing>(routes.library, undefined, { signal })
        : undefined,
    [version, online],
  );
  const [kind, setKind] = useState<LibraryKind | "">(initialKind);
  const [where, setWhere] = useState(ALL);
  const [query, setQuery] = useState("");
  const [selected, setSelected] = useState<Selection | null>(null);
  const [importing, setImporting] = useState<"memory" | "skill" | null>(null);
  const [deleting, setDeleting] = useState(false);
  const selection = useSelection();
  const moved = usePushDirection(!!selected);
  // Workspaces first, then folders, each by name, as the sidebar lists them.
  const places = [...(listing.value?.places ?? [])].sort(
    (a, b) =>
      Number(b.workspace) - Number(a.workspace) || a.name.localeCompare(b.name),
  );
  const placeOf = (path?: string) =>
    places.find((place) => place.path === path);
  const items = listing.value?.items ?? (listing.error ? [] : SAMPLE);
  const found = items.filter((item) =>
    matches(query, item.name, item.description, placeOf(item.place)?.name),
  );
  const ofKind = (item: LibraryItem, value = kind) =>
    !value || item.kind === value;
  const atPlace = (item: LibraryItem, value = where) =>
    value === ALL || item.place === value;
  const shown = found.filter((item) => ofKind(item) && atPlace(item));
  const groups = [...places.map((place) => place.path), ""].flatMap((path) => {
    const rows = shown.filter((item) => item.place === path);
    return rows.length ? [{ path, rows }] : [];
  });
  const current =
    selected && "key" in selected
      ? items.find(
          (item) => item.key === selected.key && item.place === selected.place,
        )
      : undefined;
  // A place's own controls run there; every place's run in any of them.
  const home = (path = "") =>
    path || (where !== ALL && where) || places[0]?.path || "";
  const fresh = (value: LibraryKind) =>
    setSelected({ new: value, where: where === ALL ? "" : where });
  // Picking, as the sidebar picks: in the order the list shows.
  const order = groups.flatMap(({ rows }) => rows.map(idOf));
  const picked = items.filter((item) => selection.ids.has(idOf(item)));
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a new listing
  useEffect(() => {
    if (listing.value && selection.ids.size)
      selection.keep(listing.value.items.map(idOf));
  }, [listing.value]);
  useShortcuts(
    selection.selecting
      ? {
          "select-all": () => selection.all(order),
          "delete-selected": picked.length
            ? () => setDeleting(true)
            : undefined,
          deselect: selection.clear,
        }
      : {},
  );
  // Each the way its own pane deletes it; one already gone (a doc in a
  // deck's folder) counts as gone.
  const remove = (item: LibraryItem) =>
    item.kind === "memory" || item.kind === "skill"
      ? manage(control(item.kind), {
          cwd: home(item.place),
          action: "forget",
          key: item.key,
          revision: item.revision,
        })
      : api(routes.libraryDelete, {
          kind: item.kind,
          place: item.place,
          key: item.key,
          session: item.session,
        }).catch((error) => {
          if (error?.status !== 404) throw error;
        });
  const kinds = (Object.keys(NAME) as LibraryKind[]).flatMap((value) => {
    const count = picked.filter((item) => item.kind === value).length;
    return count ? [{ value, count }] : [];
  });
  const placeLabel = (path: string) => {
    const place = placeOf(path);
    return path ? (
      <FolderLabel path={path} space={place?.workspace}>
        {place?.workspace ? place.name : undefined}
      </FolderLabel>
    ) : (
      <FolderLabel>Everywhere</FolderLabel>
    );
  };
  const rows = groups.map(({ path, rows }) => (
    <section
      key={path}
      class="library-group"
      aria-label={placeOf(path)?.name ?? "Everywhere"}
    >
      <h2 class="small">
        {placeLabel(path)}
        <span class="muted">{rows.length}</span>
      </h2>
      {rows.map((item) => {
        const Icon = ICON[item.kind ?? "memory"];
        return (
          <div
            key={idOf(item)}
            class="library-item"
            data-picked={selection.ids.has(idOf(item)) || undefined}
          >
            {selection.selecting && (
              <RowCheck
                picked={selection.ids.has(idOf(item))}
                label={`Select ${item.name}`}
                toggle={() => selection.toggle(idOf(item))}
              />
            )}
            <ListRow
              class="library-row entry"
              aria-current={
                (current?.key === item.key && current.place === item.place) ||
                undefined
              }
              onClick={(event) =>
                selection.click(idOf(item), event, order) ||
                setSelected({ key: item.key, place: item.place ?? "" })
              }
              title={
                <>
                  <Icon />
                  <DataText>{item.name}</DataText>
                </>
              }
              meta={
                <DataText>
                  {NAME[item.kind ?? "memory"]} · {detail(item)}
                  {item.status && item.status !== "available"
                    ? ` · ${item.status}`
                    : ""}
                </DataText>
              }
            >
              {item.modified ? (
                <small class="muted">
                  <Time value={item.modified} />
                </small>
              ) : null}
            </ListRow>
          </div>
        );
      })}
    </section>
  ));
  const count = (test: (item: LibraryItem) => boolean) =>
    found.filter(test).length;
  const document =
    picked.length > 0 ? (
      <section class="library-summary" aria-label="Selection">
        <h2>{`${picked.length} selected`}</h2>
        <ul>
          {kinds.map(({ value, count }) => (
            <li key={value}>
              <strong>{plural(count, NAME[value], NAMES[value])}</strong>
              <span class="muted">{` · ${GOES[value]}`}</span>
            </li>
          ))}
        </ul>
      </section>
    ) : selected && "new" in selected ? (
      <LibraryDocument
        key={`new-${selected.new}`}
        kind={control(selected.new)}
        entry={{
          new: true,
          content:
            selected.new === "skill"
              ? "---\ndescription: Describe when to use this skill\n---\n\n"
              : "",
          where: selected.where,
        }}
        cwd={home(selected.where)}
        places={places}
        online={online}
        limit={
          listing.value?.limits[selected.new === "skill" ? "skill" : "memory"]
        }
        opened={(key, place) => setSelected({ key, place })}
        close={() => setSelected(null)}
        changed={listing.retry}
      />
    ) : current?.kind === "memory" || current?.kind === "skill" ? (
      <LibraryDocument
        key={`${current.place}\n${current.key}`}
        kind={control(current.kind)}
        entry={current}
        cwd={home(current.place)}
        places={places}
        online={online}
        limit={listing.value?.limits[current.kind]}
        close={() => setSelected(null)}
        changed={listing.retry}
      />
    ) : current ? (
      <LibraryArtifact
        key={`${current.place}\n${current.key}`}
        item={current}
        place={placeOf(current.place)}
        close={() => setSelected(null)}
        open={(target) => {
          const place = placeOf(current.place);
          if (place) openFiles(place, target);
        }}
        choose={choose}
      />
    ) : null;
  return (
    <div class="management library">
      <div class="management-toolbar">
        <Input
          aria-label="Search library"
          type="search"
          placeholder="Search names, content, places…"
          value={query}
          onInput={(event) => setQuery(event.currentTarget.value)}
        />
        <Select
          class="library-place"
          aria-label="Place"
          value={where}
          onChange={(event) => setWhere(event.currentTarget.value)}
        >
          <option value={ALL}>All places</option>
          <option value="">Everywhere</option>
          {places.map((place) => (
            <option key={place.path} value={place.path} title={place.path}>
              {place.name}
            </option>
          ))}
        </Select>
        {kind === "memory" || kind === "skill" ? (
          <>
            <Button
              disabled={!online || !places.length}
              onClick={() => setImporting(kind)}
            >
              Import
            </Button>
            <Button
              variant="primary"
              disabled={!online || !places.length}
              onClick={() => fresh(kind)}
            >
              <Plus />
              New {kind}
            </Button>
          </>
        ) : (
          // Everything else is made by agents: memories and skills alike.
          (["Import", "New"] as const).map((action) => (
            <Menu
              key={action}
              label={action}
              trigger={
                <>
                  {action}
                  <ChevronDown />
                </>
              }
            >
              {(["memory", "skill"] as const).map((value) => (
                <MenuItem
                  key={value}
                  disabled={!online || !places.length}
                  onClick={() =>
                    action === "New" ? fresh(value) : setImporting(value)
                  }
                >
                  {action === "New"
                    ? value === "memory"
                      ? "Memory"
                      : "Skill"
                    : value === "memory"
                      ? "Memories…"
                      : "Skills…"}
                </MenuItem>
              ))}
            </Menu>
          ))
        )}
      </div>
      {listing.value?.limits.memory_enabled === false && (
        <p class="muted">
          Memory is disabled in configuration. Saved entries remain available to
          manage.
        </p>
      )}
      <div
        class={`management-body ${selected ? "has-selection" : ""}`}
        data-direction={moved === "back" ? "back" : undefined}
      >
        <nav class="library-rail" aria-label="Library filters">
          {RAIL.map(([heading, kinds]) => (
            <div
              key={heading}
              class="library-kinds"
              role="group"
              aria-label={heading || "All"}
            >
              {heading && <h2 class="small muted">{heading}</h2>}
              {kinds.map(([value, label]) => (
                <Button
                  key={value}
                  variant="quiet"
                  class="entry"
                  aria-current={kind === value || undefined}
                  onClick={() => setKind(value)}
                >
                  {label}
                  <small class="muted">
                    {count((item) => ofKind(item, value) && atPlace(item))}
                  </small>
                </Button>
              ))}
            </div>
          ))}
          <div class="library-where" role="group" aria-label="Places">
            <h2 class="small muted">Where</h2>
            {[ALL, "", ...places.map((place) => place.path)].map((value) => (
              <Button
                key={value}
                variant="quiet"
                class="entry"
                aria-current={where === value || undefined}
                onClick={() => setWhere(value)}
              >
                {value === ALL ? "All places" : placeLabel(value)}
                <small class="muted">
                  {count((item) => ofKind(item) && atPlace(item, value))}
                </small>
              </Button>
            ))}
          </div>
        </nav>
        <div class={moved ? "management-list push" : "management-list"}>
          {selection.selecting && (
            <SelectionBar selection={selection}>
              <Button
                variant="destructive"
                disabled={!online || !picked.length}
                onClick={() => setDeleting(true)}
              >
                Delete…
              </Button>
            </SelectionBar>
          )}
          {listing.error ? (
            <LoadError error={listing.error} retry={listing.retry} />
          ) : !listing.value ? (
            <Placeholder label="Loading library…">{rows}</Placeholder>
          ) : groups.length ? (
            rows
          ) : (
            <EmptyState>
              {query
                ? "Nothing here matches."
                : "Nothing kept here yet. Memories and skills come from conversations or Import; designs and shared files from what agents make."}
            </EmptyState>
          )}
        </div>
        {document && (
          <div class={moved ? "management-editor push" : "management-editor"}>
            {document}
          </div>
        )}
      </div>
      {deleting && (
        <Modal
          title={deleteTitle(
            kinds.map(({ value, count }) => [count, NAME[value], NAMES[value]]),
          )}
          close={() => setDeleting(false)}
        >
          <DeleteMany
            entries={picked.map((item) => ({
              key: idOf(item),
              name: `${item.name} · ${NAME[item.kind ?? "memory"]}`,
              remove: () => remove(item),
            }))}
            note={`${kinds
              .map(
                ({ value, count }) =>
                  `${plural(count, NAME[value], NAMES[value])}: ${GOES[value]}`,
              )
              .join("; ")}. `}
            online={online}
            done={() => listing.retry()}
            close={() => setDeleting(false)}
          />
        </Modal>
      )}
      {importing && (
        <ImportDialog
          cwd={home()}
          kind={control(importing)}
          close={() => {
            setImporting(null);
            listing.retry();
          }}
        />
      )}
    </div>
  );
}
