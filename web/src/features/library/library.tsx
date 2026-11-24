import {
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import type { FunctionComponent } from "preact";
import { BookOpen, ChevronDown, Brain, FileText, Plus } from "lucide-preact";
import { useApi } from "../../state/api.ts";
import { useRemoteArtifacts } from "./use-remote-artifacts.ts";
import type {
  FileTarget,
  LibraryItem,
  LibraryKind,
  LibraryListing,
  LibraryPlace,
} from "../../shared/types.ts";
import {
  Bar,
  Button,
  DataText,
  EmptyState,
  Input,
  LoadError,
  Modal,
  Placeholder,
  Select,
  Spinner,
  Time,
} from "../../shared/ui.tsx";
import { usePushDirection } from "../../shared/motion.ts";
import { bytes, plural } from "../../shared/quantities.ts";
import {
  ARTIFACT,
  type ArtifactType,
  artifactSize,
} from "../../shared/artifact.ts";
import { useResource } from "../../shared/use-resource.ts";
import { allPlaces as ALL, filterLibrary, placeId } from "./filters.ts";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import FolderLabel from "../../shared/folder-label.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { SheetButton } from "../../shared/sheet.tsx";
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

const TYPES = Object.keys(ARTIFACT) as ArtifactType[];
// Each artifact type's value, from its table.
const ofEach = <T,>(value: (type: ArtifactType) => T) =>
  Object.fromEntries(TYPES.map((type) => [type, value(type)])) as Record<
    ArtifactType,
    T
  >;
const RAIL: [string, [LibraryKind | "", string][]][] = [
  ["", [["", "Everything"]]],
  [
    "Artifacts",
    [
      ...TYPES.map((type): [LibraryKind, string] => [
        type,
        `${ARTIFACT[type].name}s`,
      ]),
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
  ...ofEach((type) => ARTIFACT[type].icon),
  file: FileText,
  memory: Brain,
  skill: BookOpen,
};
const NAME: Record<LibraryKind, string> = {
  ...ofEach((type) => ARTIFACT[type].name.toLowerCase()),
  file: "shared file",
  memory: "memory",
  skill: "skill",
};
const NAMES: Partial<Record<LibraryKind, string>> = { memory: "memories" };
// What deleting one of a kind removes; an artifact that is alone is no
// folder.
const GOES: Record<LibraryKind, string> = {
  ...ofEach(() => "deleted from the project, folder and all"),
  file: "deleted",
  memory: "forgotten",
  skill: "forgotten; supporting files stay",
};
// An item's id in the selection: its place and key.
const idOf = (item: LibraryItem) => `${item.place ?? ""}\n${item.key}`;
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
        : item.kind && item.kind in ARTIFACT
          ? artifactSize(item.kind as ArtifactType, item.count ?? 0)
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
  choose: (session: string, host?: string) => void;
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
  const list = useRef<HTMLDivElement>(null);
  const selectButton = useRef<HTMLButtonElement>(null);
  const wasSelecting = useRef(false);
  useLayoutEffect(() => {
    // Select and Done replace each other. Move focus when the old control
    // disappeared; a modifier-clicked row keeps the focus it already has.
    if (
      selection.selecting !== wasSelecting.current &&
      globalThis.document.activeElement === globalThis.document.body
    ) {
      const target = selection.selecting
        ? list.current?.querySelector<HTMLInputElement>(".row-check")
        : selectButton.current;
      target?.focus();
    }
    wasSelecting.current = selection.selecting;
  }, [selection.selecting]);
  const moved = usePushDirection(!!selected);
  // Workspaces first, then folders, each by name, as the sidebar lists them.
  const places = useMemo(
    () =>
      [...(listing.value?.places ?? [])].sort(
        (a, b) =>
          Number(b.workspace) - Number(a.workspace) ||
          a.name.localeCompare(b.name),
      ),
    [listing.value?.places],
  );
  const byPlace = useMemo(
    () => new Map(places.map((p) => [placeId(p), p])),
    [places],
  );
  const placeOf = (key?: string) => byPlace.get(key ?? "");
  // Memory and skill management runs on the main host.
  const localPlaces = places.filter((place) => !place.host);
  const canManage = online && !!localPlaces.length && !placeOf(where)?.host;
  // Session ids are stable across catalogue ticks. Each remote answer can
  // arrive on its own, while local artifacts and attachment snapshots show.
  const remotePlaces = useMemo(
    () => places.filter((place) => place.host && place.session),
    [places],
  );
  const { results: remote, retry: retryRemote } = useRemoteArtifacts(
    remotePlaces,
    online,
  );
  const items = useMemo(
    () =>
      listing.value
        ? [
            ...listing.value.items,
            ...remotePlaces.flatMap(
              (place) => remote[place.session!]?.items ?? [],
            ),
          ]
        : listing.error
          ? []
          : SAMPLE,
    [listing.value, listing.error, remotePlaces, remote],
  );
  const visibleRemote = remotePlaces.filter(
    (place) => where === ALL || placeId(place) === where,
  );
  const loadingRemote =
    online && visibleRemote.some((place) => !remote[place.session!]);
  const failedRemote = visibleRemote.filter(
    (place) => remote[place.session!]?.error,
  );
  const {
    groups,
    kinds: kindCounts,
    locations,
  } = useMemo(
    () => filterLibrary(items, byPlace, kind, where, query),
    [items, byPlace, kind, where, query],
  );
  const current =
    selected && "key" in selected
      ? items.find(
          (item) => item.key === selected.key && item.place === selected.place,
        )
      : undefined;
  // A place's own controls run there; every place's run in any of them.
  const home = (path = "") =>
    path ||
    (where !== ALL && !placeOf(where)?.host && where) ||
    localPlaces[0]?.path ||
    "";
  const fresh = (value: LibraryKind) =>
    setSelected({ new: value, where: where === ALL ? "" : where });
  // Picking, as the sidebar picks: in the order the list shows.
  const order = groups.flatMap(({ rows }) => rows.map(idOf));
  const picked = items.filter((item) => selection.ids.has(idOf(item)));
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a new listing
  useEffect(() => {
    if (listing.value && selection.ids.size) selection.keep(items.map(idOf));
  }, [listing.value, remote]);
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
          session_id: item.session,
        }).catch((error) => {
          if (error?.status !== 404) throw error;
        });
  const kinds = (Object.keys(NAME) as LibraryKind[]).flatMap((value) => {
    const of = picked.filter((item) => item.kind === value);
    const goes = of[0]?.alone ? "deleted from the project" : GOES[value];
    return of.length ? [{ value, count: of.length, goes }] : [];
  });
  const placeLabel = (path: string) => {
    const place = placeOf(path);
    return path ? (
      <FolderLabel path={place?.path ?? path} space={place?.workspace}>
        {place?.workspace || place?.host ? place.name : undefined}
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
  const document =
    picked.length > 0 ? (
      <section class="library-summary" aria-label="Selection">
        <h2>{`${picked.length} selected`}</h2>
        <ul>
          {kinds.map(({ value, count, goes }) => (
            <li key={value}>
              <strong>{plural(count, NAME[value], NAMES[value])}</strong>
              <span class="muted">{` · ${goes}`}</span>
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
        places={localPlaces}
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
        places={localPlaces}
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
        choose={(id) => choose(id, placeOf(current.place)?.host || "")}
      />
    ) : null;
  return (
    <div class="management library">
      <Bar edge="bottom" class="management-toolbar">
        <Input
          aria-label="Search library"
          type="search"
          placeholder="Search names, content, places…"
          value={query}
          onInput={(event) => setQuery(event.currentTarget.value)}
        />
        <SheetButton
          className="library-filters"
          label="Filter library"
          heading="Library filters"
          trigger={
            [
              kind && NAME[kind],
              where === ALL ? "" : placeOf(where)?.name || "Everywhere",
            ]
              .filter(Boolean)
              .join(" · ") || "Filters"
          }
        >
          <label>
            Kind
            <Select
              aria-label="Kind"
              value={kind}
              onChange={(event) =>
                setKind(event.currentTarget.value as LibraryKind | "")
              }
            >
              {RAIL.flatMap(([, kinds]) =>
                kinds.map(([value, name]) => (
                  <option key={value} value={value}>
                    {name}
                  </option>
                )),
              )}
            </Select>
          </label>
          <label>
            Place
            <Select
              aria-label="Place"
              value={where}
              onChange={(event) => setWhere(event.currentTarget.value)}
            >
              <option value={ALL}>All places</option>
              <option value="">Everywhere</option>
              {places.map((place) => (
                <option
                  key={placeId(place)}
                  value={placeId(place)}
                  title={place.path}
                >
                  {place.name}
                </option>
              ))}
            </Select>
          </label>
        </SheetButton>
        {kind === "memory" || kind === "skill" ? (
          <>
            <Button disabled={!canManage} onClick={() => setImporting(kind)}>
              Import
            </Button>
            <Button
              variant="primary"
              disabled={!canManage}
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
                  disabled={!canManage}
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
      </Bar>
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
                  <small class="muted">{kindCounts.get(value) ?? 0}</small>
                </Button>
              ))}
            </div>
          ))}
          <div class="library-where" role="group" aria-label="Places">
            <h2 class="small muted">Where</h2>
            {[ALL, "", ...places.map(placeId)].map((value) => (
              <Button
                key={value}
                variant="quiet"
                class="entry"
                aria-current={where === value || undefined}
                onClick={() => setWhere(value)}
              >
                {value === ALL ? "All places" : placeLabel(value)}
                <small class="muted">{locations.get(value) ?? 0}</small>
              </Button>
            ))}
          </div>
        </nav>
        <div
          class={moved ? "management-list push" : "management-list"}
          ref={list}
        >
          {selection.selecting ? (
            <SelectionBar selection={selection}>
              <Button
                variant="destructive"
                disabled={!online || !picked.length}
                onClick={() => setDeleting(true)}
              >
                Delete…
              </Button>
            </SelectionBar>
          ) : (
            listing.value &&
            groups.length > 0 && (
              <Button
                variant="quiet"
                size="compact"
                buttonRef={selectButton}
                onClick={() => selection.start()}
              >
                Select
              </Button>
            )
          )}
          {listing.error ? (
            <LoadError error={listing.error} retry={listing.retry} />
          ) : !listing.value ? (
            <Placeholder label="Loading library…">{rows}</Placeholder>
          ) : groups.length ? (
            rows
          ) : loadingRemote || failedRemote.length ? null : (
            <EmptyState>
              {query
                ? "Nothing here matches."
                : "Nothing kept here yet. Memories and skills come from conversations or Import; designs and shared files from what agents make."}
            </EmptyState>
          )}
          {loadingRemote && <Spinner label="Loading remote artifacts…" />}
          {failedRemote.map((place) => (
            <LoadError
              key={placeId(place)}
              error={`${place.name} · ${remote[place.session!].error}`}
              retry={() => retryRemote(place.session!)}
            />
          ))}
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
                ({ value, count, goes }) =>
                  `${plural(count, NAME[value], NAMES[value])}: ${goes}`,
              )
              .join("; ")}. `}
            online={online}
            done={() => {
              listing.retry();
              const changed = new Set(
                picked.flatMap((item) => {
                  const place = placeOf(item.place);
                  return item.kind !== "file" && place?.host && place.session
                    ? [place.session]
                    : [];
                }),
              );
              for (const id of changed) retryRemote(id);
            }}
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
