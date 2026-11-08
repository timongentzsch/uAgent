import { useState } from "preact/hooks";
import type { FunctionComponent } from "preact";
import {
  BookOpen,
  Brain,
  FileText,
  LayoutDashboard,
  Palette,
  Plus,
} from "lucide-preact";
import { api, routes } from "../../state/api.ts";
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
  Placeholder,
  Select,
  Time,
} from "../../shared/ui.tsx";
import { usePushDirection } from "../../shared/motion.ts";
import { bytes } from "../../shared/quantities.ts";
import { useResource } from "../../shared/use-resource.ts";
import { matches } from "../../shared/fuzzy.ts";
import FolderLabel from "../../shared/folder-label.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import ImportDialog from "./import.tsx";
import LibraryDocument from "./document.tsx";
import LibraryArtifact from "./artifact.tsx";
import "./management.css";

// The Library: everything kept across every folder and workspace, in one
// list grouped by place, as the sidebar groups conversations. A rail
// filters it by kind and place; an item opens beside the list (over it on
// a phone). Nothing is stored here: each item stays where it lives.

const RAIL: [string, [LibraryKind | "", string][]][] = [
  ["", [["", "Everything"]]],
  [
    "Artifacts",
    [
      ["design", "Designs"],
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
  system: Palette,
  file: FileText,
  memory: Brain,
  skill: BookOpen,
};
const NAME: Record<LibraryKind, string> = {
  design: "design",
  system: "design system",
  file: "shared file",
  memory: "memory",
  skill: "skill",
};
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
    : item.kind === "design"
      ? `${item.count} ${item.count === 1 ? "page" : "pages"}`
      : item.kind === "file"
        ? bytes(item.bytes ?? 0)
        : item.description || item.source;
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
  const [importing, setImporting] = useState(false);
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
  const knowledge = kind === "skill" ? "skill" : "memory";
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
          <ListRow
            key={`${item.place}\n${item.key}`}
            class="library-row entry"
            aria-current={
              (current?.key === item.key && current.place === item.place) ||
              undefined
            }
            onClick={() =>
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
        );
      })}
    </section>
  ));
  const count = (test: (item: LibraryItem) => boolean) =>
    found.filter(test).length;
  const document =
    selected && "new" in selected ? (
      <LibraryDocument
        key={`new-${selected.new}`}
        kind={selected.new === "skill" ? "skills" : "memory"}
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
        kind={current.kind === "skill" ? "skills" : "memory"}
        entry={current}
        cwd={home(current.place)}
        places={places}
        listed={current}
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
        <Button
          disabled={!online || !places.length}
          onClick={() => setImporting(true)}
        >
          Import
        </Button>
        <Button
          variant="primary"
          disabled={!online || !places.length}
          onClick={() => fresh(knowledge)}
        >
          <Plus />
          New {knowledge}
        </Button>
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
      {importing && (
        <ImportDialog
          cwd={home()}
          kind={kind === "skill" ? "skills" : "memory"}
          close={() => {
            setImporting(false);
            listing.retry();
          }}
        />
      )}
    </div>
  );
}
