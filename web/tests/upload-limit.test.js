// A file the host refused by its size reads as too large in both shapes the
// host refuses it: a 413, or a connection dropped mid-body.
import { test } from "node:test";
import assert from "node:assert/strict";
import { tooLarge, uploadLimit } from "../src/shared/limits.ts";

const MiB = 1024 * 1024;
const sized = (size) => ({ name: "big.bin", size });
const refused = (fields) => Object.assign(new Error("Load failed"), fields);

test("a 413 is too large, with the limit when the file is over it", () => {
  assert.equal(
    tooLarge(refused({ status: 413 }), sized(11 * MiB), 10 * MiB).message,
    "big.bin is too large to attach: at most 10 MiB.",
  );
  assert.equal(
    tooLarge(refused({ status: 413 }), sized(9 * MiB), undefined).message,
    "big.bin is too large to attach.",
  );
});

test("a dropped connection is too large only for a file over the limit", () => {
  assert.equal(
    tooLarge(refused({ network: true }), sized(11 * MiB), 10 * MiB).message,
    "big.bin is too large to attach: at most 10 MiB.",
  );
  const away = refused({ network: true });
  assert.equal(tooLarge(away, sized(9 * MiB), 10 * MiB), away);
  assert.equal(tooLarge(away, sized(11 * MiB), undefined), away);
});

test("a host that serves no limit is left to refuse a file itself", () => {
  assert.equal(uploadLimit([sized(50 * MiB)], 0, undefined), null);
  assert.match(
    uploadLimit([sized(11 * MiB)], 0, 10 * MiB).message,
    /at most 10 MiB each/,
  );
});
