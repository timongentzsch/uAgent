// The person's own stylesheet (~/.uagent/config/style.css), which overrides
// the design tokens. It goes in after the app's own sheets. The agent edits
// it during a turn and a person can edit it any time, so it is read again
// when the host connects and whenever a turn starts or ends; the answer is
// revalidated, so an unchanged sheet costs a 304.
import { routes } from "../state/api.ts";

let sheet: HTMLStyleElement | undefined;
let revision = 0;

export async function refreshUserStyle() {
  const requested = ++revision;
  let css: string;
  try {
    const response = await fetch(routes.userStyle, { cache: "no-cache" });
    if (!response.ok) return;
    css = await response.text();
  } catch {
    return;
  }
  // A slow response body must not undo a later refresh, even if it failed.
  if (requested !== revision || sheet?.textContent === css) return;
  sheet ??= document.createElement("style");
  sheet.textContent = css;
  document.head.append(sheet);
}
