import { type ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import { ChevronDown, ChevronRight, Plus } from "lucide-preact";
import { ApiContext, hostApi } from "../../state/api.ts";
import { HostContext, useHostContext } from "../../state/host-context.ts";
import { useHosts, type Source } from "../../state/sources.ts";
import {
  Button,
  IconButton,
  Input,
  LoadError,
  Modal,
} from "../../shared/ui.tsx";
import { Menu } from "../../shared/menu.tsx";
import FolderLabel, { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import type { Pin, Session } from "../../shared/types.ts";
import { PinItem } from "./pins.tsx";
import { collapsed, opened, placeKey, useFolding } from "./folding.ts";

export function Fold({
  name,
  label,
  children,
  closed = false,
}: {
  name: string;
  label: string;
  children: ComponentChildren;
  closed?: boolean;
}) {
  useFolding();
  const folded = closed ? !opened.has(name) : collapsed.has(name);
  return (
    <div class="list-group" role="group" aria-label={label}>
      <Button
        variant="quiet"
        class="section-fold caption"
        aria-expanded={!folded}
        onClick={() =>
          closed ? opened.set(name, folded) : collapsed.set(name, !folded)
        }
      >
        {folded ? <ChevronRight /> : <ChevronDown />}
        {label}
      </Button>
      {!folded && children}
    </div>
  );
}
function SourcePin({ source, pin }: { source: Source; pin: Pin }) {
  const current = useHostContext();
  if (!source.state) return null;
  return (
    <ApiContext.Provider value={hostApi(source.id)}>
      <HostContext.Provider
        value={{
          ...current,
          online: source.state.online,
          pins: source.state.catalogue.pins || [],
          report: source.state.report,
        }}
      >
        <Menu label={`Menu for ${pin.id}`}>
          <PinItem pin={pin} />
        </Menu>
      </HostContext.Provider>
    </ApiContext.Provider>
  );
}
function Shortcut({
  source,
  session,
  pinned = false,
}: {
  source: Source;
  session: Session;
  pinned?: boolean;
}) {
  const hosts = useHosts()!;
  return (
    <div class="session-row entry">
      <Button
        variant="quiet"
        class="session source-shortcut"
        onClick={() => hosts.open(source.id, session.id)}
      >
        <span class="truncate">{session.title || "Untitled"}</span>
      </Button>
      {pinned && (
        <SourcePin source={source} pin={{ kind: "session", id: session.id }} />
      )}
    </div>
  );
}
export function SharedShortcuts({
  activePins,
}: {
  activePins: ComponentChildren;
}) {
  const hosts = useHosts();
  if (!hosts) return activePins;
  const pinned = hosts.sources.filter(
    (source) => source.state?.catalogue.pins?.length,
  );
  const recent = hosts.sources
    .flatMap((source) =>
      (source.state?.listed || [])
        .filter(
          (session) =>
            !session.parent &&
            !source.state?.catalogue.pins?.some(
              (pin) => pin.kind === "session" && pin.id === session.id,
            ),
        )
        .map((session) => ({ source, session })),
    )
    .sort((a, b) => (b.session.updated || 0) - (a.session.updated || 0))
    .slice(0, 10);
  return (
    <>
      {!!pinned.length && (
        <Fold name="$pinned" label="Pinned">
          {pinned.map((source) => (
            <div key={source.id}>
              {hosts.sources.length > 1 && (
                <p class="source-label caption muted">{source.name}</p>
              )}
              {source.id === hosts.active
                ? activePins
                : source.state?.catalogue.pins?.map((pin) => {
                    const session = source.state?.listed.find(
                      (item) => item.id === pin.id,
                    );
                    if (pin.kind === "session")
                      return session ? (
                        <Shortcut
                          key={pin.id}
                          source={source}
                          session={session}
                          pinned
                        />
                      ) : null;
                    const terminal = source.state?.catalogue.terminals?.find(
                      (item) => item.id === pin.id,
                    );
                    return (
                      <div key={pin.id} class="session-row entry">
                        <Button
                          variant="quiet"
                          class="session source-shortcut"
                          onClick={() =>
                            hosts.open(
                              source.id,
                              "",
                              terminal?.folder || pin.id,
                              terminal ? { terminal: terminal.id } : undefined,
                            )
                          }
                        >
                          <span class="truncate">
                            {terminal?.title || folderName(pin.id)}
                          </span>
                        </Button>
                        <SourcePin source={source} pin={pin} />
                      </div>
                    );
                  })}
            </div>
          ))}
        </Fold>
      )}
      {!!recent.length && (
        <Fold name="$recent" label="Recent" closed>
          {recent.map(({ source, session }) => (
            <div key={`${source.id}:${session.id}`}>
              <Shortcut source={source} session={session} />
              {hosts.sources.length > 1 && (
                <span class="recent-source caption muted">
                  {source.name} · {folderName(folderOf(session))}
                </span>
              )}
            </div>
          ))}
        </Fold>
      )}
    </>
  );
}
export function OtherPlaces({ source }: { source: Source }) {
  const hosts = useHosts()!;
  const state = source.state;
  const sessions = state?.listed || [];
  const spaces = state?.catalogue.spaces || [];
  const pins = state?.catalogue.pins || [];
  const paths = [
    ...new Set([
      ...spaces.map((space) => space.path),
      ...sessions.map(folderOf),
    ]),
  ];
  const title = (path: string) =>
    spaces.find((space) => space.path === path)?.name || folderName(path);
  useFolding();
  return (
    <Fold name={`$host:${source.id}`} label={source.name}>
      {paths
        .filter(
          (path) =>
            !pins.some((pin) => pin.kind === "folder" && pin.id === path),
        )
        .map((path) => {
          const name = title(path),
            key = placeKey(source.id, path),
            folded = collapsed.has(key);
          return (
            <section key={path}>
              <div class="place-head entry">
                <IconButton
                  label={`Conversations of ${source.name}: ${name}`}
                  aria-expanded={!folded}
                  onClick={() => collapsed.set(key, !folded)}
                >
                  {folded ? <ChevronRight /> : <ChevronDown />}
                </IconButton>
                <h2>
                  <Button
                    variant="quiet"
                    onClick={() => hosts.open(source.id, "", path)}
                  >
                    <FolderLabel
                      path={path}
                      space={spaces.some((space) => space.path === path)}
                    >
                      {title(path)}
                    </FolderLabel>
                  </Button>
                </h2>
              </div>
              {!folded && (
                <div class="place-body">
                  {sessions
                    .filter(
                      (session) =>
                        folderOf(session) === path &&
                        !session.parent &&
                        !pins.some(
                          (pin) =>
                            pin.kind === "session" && pin.id === session.id,
                        ),
                    )
                    .map((session) => (
                      <Shortcut
                        key={session.id}
                        source={source}
                        session={session}
                      />
                    ))}
                </div>
              )}
            </section>
          );
        })}
      {!paths.length && (
        <Button
          variant="quiet"
          class="entry"
          onClick={() => hosts.open(source.id)}
        >
          {source.phase === "connected"
            ? "Open host"
            : "Connect to browse folders"}
        </Button>
      )}
    </Fold>
  );
}
export function HostList() {
  const hosts = useHosts();
  const [detail, setDetail] = useState<string | null>(null);
  const [adding, setAdding] = useState(false);
  const [address, setAddress] = useState("");
  const [folder, setFolder] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  if (!hosts) return null;
  const source = hosts.sources.find((source) => source.id === detail);
  const run = async (action: string, id?: string) => {
    setBusy(true);
    setError(null);
    try {
      const added = await hosts.action(
        action,
        id,
        action === "add" ? address.trim() : undefined,
      );
      if (action === "add") {
        setAdding(false);
        setAddress("");
        setDetail(added);
      }
    } catch (error) {
      setError(error);
    } finally {
      setBusy(false);
    }
  };
  const status = (source: Source) =>
    !hosts.gatewayOnline && source.id
      ? "Unavailable"
      : {
          connected: "Connected",
          connecting: "Connecting…",
          reconnecting: "Reconnecting…",
          disconnected: "Disconnected",
        }[source.phase];
  return (
    <div class="host-list">
      <Fold name="$hosts" label="Hosts">
        <div class="host-list-scroll">
          {hosts.sources.map((source) => (
            <Button
              key={source.id}
              variant="quiet"
              class="host-row entry"
              aria-current={hosts.active === source.id ? "true" : undefined}
              onClick={() => {
                setError(null);
                setDetail(source.id);
              }}
            >
              <span
                class="host-square"
                data-connected={
                  (hosts.gatewayOnline && source.phase === "connected") ||
                  undefined
                }
                aria-hidden="true"
              />
              <span class="truncate">{source.name}</span>
              <span class="host-phase caption">{status(source)}</span>
            </Button>
          ))}
        </div>
        <Button
          variant="quiet"
          class="entry"
          disabled={!hosts.gatewayOnline}
          onClick={() => {
            setError(null);
            setAdding(true);
          }}
        >
          <Plus />
          Add host
        </Button>
      </Fold>
      {(adding || source) && (
        <Modal
          title={adding ? "Add remote host" : source!.name}
          close={() => {
            setAdding(false);
            setDetail(null);
          }}
        >
          {adding ? (
            <form
              onSubmit={(event) => {
                event.preventDefault();
                void run("add");
              }}
            >
              <label>
                SSH alias or user@machine
                <Input
                  value={address}
                  autoFocus
                  onInput={(event) => setAddress(event.currentTarget.value)}
                  placeholder="devbox or user@machine"
                />
              </label>
              <p class="small muted">
                Uses SSH access configured on the serving host. The remote agent
                runs beside your files.
              </p>
              <Button
                variant="primary"
                busy={busy}
                disabled={!address.trim() || !hosts.gatewayOnline}
              >
                Connect
              </Button>
            </form>
          ) : (
            <>
              <p>
                {source!.address} · {status(source!)}
              </p>
              {!hosts.gatewayOnline && (
                <p>The serving host is unavailable. Reconnect to it first.</p>
              )}
              {source!.error && <p class="host-error small">{source!.error}</p>}
              <div class="host-actions">
                <Button
                  disabled={!hosts.gatewayOnline}
                  onClick={() => {
                    hosts.open(source!.id);
                    setDetail(null);
                  }}
                >
                  Open host
                </Button>
                {!!source!.id && (
                  <>
                    <Button
                      disabled={!hosts.gatewayOnline}
                      busy={busy}
                      onClick={() => void run("reconnect", source!.id)}
                    >
                      Reconnect
                    </Button>
                    <Button
                      disabled={
                        !hosts.gatewayOnline || source!.phase === "disconnected"
                      }
                      onClick={() => void run("disconnect", source!.id)}
                    >
                      Disconnect
                    </Button>
                  </>
                )}
              </div>
              <form
                onSubmit={(event) => {
                  event.preventDefault();
                  hosts.open(source!.id, "", folder.trim());
                  setDetail(null);
                  setFolder("");
                }}
              >
                <label>
                  Folder path
                  <Input
                    value={folder}
                    placeholder="/home/user/project"
                    onInput={(event) => setFolder(event.currentTarget.value)}
                  />
                </label>
                <Button
                  disabled={
                    !hosts.gatewayOnline || !folder.trim().startsWith("/")
                  }
                >
                  Open folder
                </Button>
              </form>
              {!!source!.id && (
                <p class="small muted">
                  Disconnect closes SSH access. Work continues on the remote
                  host. Reconnect restores the catalogue before enabling
                  actions.
                </p>
              )}
            </>
          )}
          {error && <LoadError error={error} />}
        </Modal>
      )}
    </div>
  );
}
