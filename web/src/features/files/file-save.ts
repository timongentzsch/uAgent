// What a person changes by hand in a folder's files, written one at a time,
// each into the file as it then is. Each write is kept, to be undone and
// made again while the file is as the last write from here left it.

interface Change {
  path: string;
  before: string;
  after: string;
}

export function fileSaver(
  io: {
    read: (path: string) => Promise<{ text: string; revision?: string }>;
    write: (
      path: string,
      text: string,
      revision: string,
      closing: boolean,
    ) => Promise<{ revision: string }>;
  },
  // Said each time what can be undone or made again changes.
  changed = () => {},
) {
  // The file as last read or written here: a closing page cannot wait for a
  // read, and writes into this.
  let seen: { path: string; text: string; revision: string } | undefined;
  // The revision a write from here left each file at.
  const left = new Map<string, string>();
  const past: Change[] = [];
  const undone: Change[] = [];
  let last: Promise<unknown> = Promise.resolve();
  const queue = <T>(run: () => Promise<T>) => {
    const made = last.then(run);
    last = made.catch(() => {});
    return made;
  };
  const drop = (path: string, from: Change[]) =>
    from.splice(0, from.length, ...from.filter((was) => was.path !== path));
  // The file now. Written since by someone else, what was written here
  // before is not this file's to undo.
  const read = async (path: string, closing: boolean) => {
    if (!closing) {
      const { text, revision = "" } = await io.read(path);
      seen = { path, text, revision };
    }
    if (seen?.path !== path || !seen.revision)
      throw new Error("the file was not read here.");
    if (left.has(path) && left.get(path) !== seen.revision) {
      left.delete(path);
      drop(path, past);
      drop(path, undone);
      changed();
    }
    return seen;
  };
  const put = async (path: string, text: string, closing = false) => {
    const { revision } = await io.write(path, text, seen!.revision, closing);
    seen = { path, text, revision };
    left.set(path, revision);
  };
  return {
    // Writes what `change` makes of the file's text, which throws where it
    // cannot be made; whether that is another text.
    write: (path: string, change: (text: string) => string, closing = false) =>
      queue(async () => {
        const { text } = await read(path, closing);
        const after = change(text);
        if (after === text) return false;
        await put(path, after, closing);
        drop(path, undone);
        past.push({ path, before: text, after });
        changed();
        return true;
      }),
    // The last change to `path` undone, or `redo` the last undone made
    // again; whether one was.
    undo: (path: string, redo = false) =>
      queue(async () => {
        const [from, to] = redo ? [undone, past] : [past, undone];
        if (!from.some((was) => was.path === path)) return false;
        await read(path, false);
        const at = from.findLastIndex((was) => was.path === path);
        if (at < 0)
          throw new Error(
            `Not ${redo ? "redone" : "undone"}: the file changed meanwhile.`,
          );
        const change = from[at];
        await put(path, redo ? change.after : change.before);
        to.push(...from.splice(at, 1));
        changed();
        return true;
      }),
    can: (path: string) => ({
      undo: past.some((was) => was.path === path),
      redo: undone.some((was) => was.path === path),
    }),
  };
}
