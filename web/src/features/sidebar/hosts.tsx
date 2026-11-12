import { type ComponentChildren } from "preact";
import { useMemo, useState } from "preact/hooks";
import { useAction } from "../../shared/use-action.ts";
import {
  connectionLabels,
  StatusLed,
} from "../../shared/connection-status.tsx";
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
  Notice,
} from "../../shared/ui.tsx";
import HostLatency from "./host-latency.tsx";
import { Menu } from "../../shared/menu.tsx";
import FolderLabel, { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import type { Pin, Session } from "../../shared/types.ts";
import { livePins, PinItem } from "./pins.tsx";
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
function sourcePins(source: Source) {
  const { listed, catalogue } = source.state;
  return livePins(
    catalogue.pins,
    listed,
    catalogue.spaces,
    catalogue.terminals || [],
  );
}
function SourcePin({
  source,
  pin,
  pins,
}: {
  source: Source;
  pin: Pin;
  pins: Pin[];
}) {
  const current = useHostContext();
  return (
    <ApiContext.Provider value={hostApi(source.id)}>
      <HostContext.Provider value={{ ...current, pins }}>
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
  pins,
}: {
  source: Source;
  session: Session;
  pins?: Pin[];
}) {
  const hosts = useHosts();
  return (
    <div class="session-row entry">
      <Button
        variant="quiet"
        class="session source-shortcut"
        onClick={() => hosts.open(source.id, session.id)}
      >
        <span class="truncate">{session.title || "Untitled"}</span>
      </Button>
      {pins && (
        <SourcePin
          source={source}
          pin={{ kind: "session", id: session.id }}
          pins={pins}
        />
      )}
    </div>
  );
}
export function SharedShortcuts({
  activePins,
}: {
  activePins: ComponentChildren[];
}) {
  const hosts = useHosts();
  const sources = useMemo(
    () =>
      hosts.sources.map((source) => {
        const pins = sourcePins(source);
        return {
          source,
          pins,
          sessions: new Set(
            pins.filter((pin) => pin.kind === "session").map((pin) => pin.id),
          ),
        };
      }),
    [hosts.sources],
  );
  // The active host's pins are the rows the list itself draws.
  const pinned = sources.filter(({ source, pins }) =>
    source.id === hosts.active ? activePins.length : pins.length,
  );
  const recent = sources
    .flatMap(({ source, sessions }) =>
      source.state.listed
        .filter((session) => !session.parent && !sessions.has(session.id))
        .map((session) => ({ source, session })),
    )
    .sort((a, b) => (b.session.updated || 0) - (a.session.updated || 0))
    .slice(0, 10);
  return (
    <>
      {!!pinned.length && (
        <Fold name="$pinned" label="Pinned">
          {pinned.map(({ source, pins }) => (
            <div key={source.id}>
              {hosts.sources.length > 1 && (
                <p class="source-label caption">{source.name}</p>
              )}
              {source.id === hosts.active
                ? activePins
                : pins.map((pin) => {
                    const session = source.state.listed.find(
                      (item) => item.id === pin.id,
                    );
                    if (pin.kind === "session")
                      return session ? (
                        <Shortcut
                          key={pin.id}
                          source={source}
                          session={session}
                          pins={pins}
                        />
                      ) : null;
                    const terminal = source.state.catalogue.terminals?.find(
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
                        <SourcePin source={source} pin={pin} pins={pins} />
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
                <span class="recent-source caption">
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
  const hosts = useHosts();
  const { listed: sessions, catalogue } = source.state;
  const spaces = catalogue.spaces || [];
  const pins = catalogue.pins || [];
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
          onClick={() =>
            hosts.open(source.id, "", "", { create: "conversation" })
          }
        >
          {source.phase === "connected"
            ? "New conversation…"
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
  const { run, busy, error, setError } = useAction();
  const source = hosts.sources.find((source) => source.id === detail);
  const act = (action: string, id?: string) =>
    run(async () => {
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
    });
  const status = (source: Source) =>
    hosts.gatewayOnline ? connectionLabels[source.phase] : "Unavailable";
  return (
    <div class="host-list">
      <Fold name="$hosts" label="Remote hosts">
        <div class="host-list-scroll">
          {hosts.sources
            .filter((source) => source.id)
            .map((source) => (
              <Button
                key={source.id}
                variant="quiet"
                class="host-row entry"
                onClick={() => {
                  setError(null);
                  setDetail(source.id);
                }}
              >
                <StatusLed
                  state={
                    hosts.gatewayOnline && source.phase === "connected"
                      ? "active"
                      : "idle"
                  }
                />
                <span class="truncate">{source.name}</span>
                <span class="host-phase caption">
                  {status(source)}
                  {source.phase === "connected" && source.round_trip_ms != null
                    ? ` · ${Math.round(source.round_trip_ms)} ms`
                    : ""}
                </span>
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
                void act("add");
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
                Uses SSH access configured on the serving host. The remote
                gateway executes file and command requests beside your files.
              </p>
              <Button
                type="submit"
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
                <Notice tone="warning">
                  The serving host is unavailable. Reconnect to it first.
                </Notice>
              )}
              {source!.error && <Notice tone="error">{source!.error}</Notice>}
              {source!.phase === "connected" && hosts.gatewayOnline && (
                <HostLatency key={source!.id} id={source!.id} />
              )}
              <div class="host-actions">
                <Button
                  disabled={
                    busy ||
                    !hosts.gatewayOnline ||
                    source!.phase === "connected" ||
                    source!.phase === "connecting"
                  }
                  onClick={() => void act("reconnect", source!.id)}
                >
                  Reconnect
                </Button>
                <Button
                  disabled={
                    busy ||
                    !hosts.gatewayOnline ||
                    source!.phase === "disconnected"
                  }
                  onClick={() => void act("disconnect", source!.id)}
                >
                  Disconnect
                </Button>
              </div>
              <p class="small muted">
                Disconnect closes SSH access. Work continues on the remote host.
                Reconnect restores remote file and command access.
              </p>
            </>
          )}
          {error && <LoadError error={error} />}
        </Modal>
      )}
    </div>
  );
}
