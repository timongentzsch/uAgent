// A chunk loaded once and shared by every caller. A load that failed is not
// kept: the next call tries again, so a moment offline does not last.
export function once<T>(load: () => Promise<T>) {
  let held: Promise<T> | undefined;
  return () =>
    (held ??= load().catch((failure) => {
      held = undefined;
      throw failure;
    }));
}

// A load started ahead of its use: the use loads again and reports a failure.
export const warm = (...loads: Promise<unknown>[]) => {
  for (const load of loads) load.catch(() => {});
};
