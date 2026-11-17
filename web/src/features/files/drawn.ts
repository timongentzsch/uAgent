import { saveLink } from "../../shared/share.ts";

// What the host draws of a file (a PNG or a PDF), saved or copied once what
// was typed is in the file: `settle` writes it.
export type Drawn = (as: "png" | "pdf", path: string) => string;

export const saveDrawn = (
  settle: () => Promise<unknown>,
  address: string,
  name: string,
) => settle().then(() => saveLink(`${address}&download=1`, name));

export const copyDrawn = (settle: () => Promise<unknown>, address: string) =>
  navigator.clipboard.write([
    new ClipboardItem({
      "image/png": settle()
        .then(() => fetch(address))
        .then((response) => response.blob()),
    }),
  ]);
