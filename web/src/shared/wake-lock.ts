import { useEffect } from "preact/hooks";
import { coarsePointer } from "./layout.ts";

// Keeps a touch screen on while `active`, so a phone does not lock mid-turn.
// A laptop is left to sleep. The browser drops the lock when the page hides,
// so it is asked for again on return. A refusal is not worth reporting.
export function useWakeLock(active: boolean) {
  useEffect(() => {
    if (!active || !coarsePointer() || !navigator.wakeLock) return;
    let lock: WakeLockSentinel | undefined;
    let stopped = false;
    const request = () => {
      if (document.visibilityState !== "visible" || (lock && !lock.released))
        return;
      navigator.wakeLock.request("screen").then(
        (granted) => {
          if (stopped) void granted.release();
          else lock = granted;
        },
        () => {},
      );
    };
    request();
    document.addEventListener("visibilitychange", request);
    return () => {
      stopped = true;
      document.removeEventListener("visibilitychange", request);
      void lock?.release();
    };
  }, [active]);
}
