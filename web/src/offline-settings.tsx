import { useEffect, useState } from "preact/hooks";
import { offline, type Download } from "./offline.ts";
import { bytes } from "./quantities.ts";
import { LoadError, Toggle } from "./ui.tsx";
export function useDownloads() {
  const [downloads, setDownloads] = useState<Download[]>([]);
  useEffect(() => {
    let active = true;
    const refresh = () =>
      offline
        .downloads()
        .then((value) => {
          if (active) setDownloads(value);
        })
        .catch(() => {});
    refresh();
    addEventListener("uagent-offline-change", refresh);
    return () => {
      active = false;
      removeEventListener("uagent-offline-change", refresh);
    };
  }, []);
  return downloads;
}
export default function OfflineSettings() {
  const downloads = useDownloads();
  const saved = downloads.filter((item) => item.pinned).length;
  const [error, setError] = useState<unknown>();
  const [enabled, setEnabled] = useState(offline.enabled);
  return (
    <div class="settings-fields">
      <Toggle
        label="Available offline"
        help="Keep conversations you open on this device. Online content refreshes automatically."
        checked={enabled}
        onChange={async (event) => {
          const value = event.currentTarget.checked;
          try {
            await offline.toggle(value);
            setEnabled(value);
          } catch (error) {
            setError(error);
          }
        }}
      />
      <div class="dialog-actions">
        <span class="muted small">
          {bytes(downloads.reduce((sum, item) => sum + item.bytes, 0))} /{" "}
          {bytes(50 * 1024 * 1024)} · {saved} saved conversation
          {saved === 1 ? "" : "s"}
        </span>
        <button onClick={() => offline.clear().catch(setError)}>
          Clear downloads and drafts
        </button>
      </div>
      {downloads
        .filter((item) => item.pinned)
        .map((item) => (
          <div class="dialog-actions" key={item.id}>
            <span class="small">
              {item.title || item.id} ·{" "}
              {item.complete
                ? "Retained history available"
                : "Partial download"}
            </span>
            <button onClick={() => offline.remove(item.id).catch(setError)}>
              Remove download
            </button>
          </div>
        ))}
      {error && <LoadError error={error} />}
    </div>
  );
}
