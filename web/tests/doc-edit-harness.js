import { blockSaver } from "../src/features/files/doc-edit.ts";

export const doc = (...texts) =>
  `<!doctype html>\n<html><head></head><body>${texts.map((text) => `<p>${text}</p>`).join("")}</body></html>`;
export const edit = (index, before, html) => ({ index, before, html });
// What a save came to: what it answered, or why it was refused.
export const outcome = (made) =>
  made.then(
    (saved) => saved,
    (error) => error.message,
  );

// The real saver over a file of two paragraphs kept here: `calls` are what
// it read and wrote, and `refuse` makes its next writes fail.
export function saverOn() {
  const file = { text: doc("a", "b"), revision: 0 };
  const calls = [];
  let refused = 0;
  const saver = blockSaver({
    read: async () => {
      calls.push(`read ${file.revision}`);
      return { text: file.text, revision: String(file.revision) };
    },
    write: async (_path, text, revision, closing) => {
      calls.push(`write on ${revision}${closing ? ", closing" : ""}`);
      if (revision !== String(file.revision) || refused-- > 0)
        throw new Error("refused");
      file.text = text;
      return { revision: String(++file.revision) };
    },
  });
  return {
    saver,
    file,
    calls,
    refuse: (times) => {
      refused = times;
    },
    body: () => /<body>(.*)<\/body>/.exec(file.text)[1],
  };
}
