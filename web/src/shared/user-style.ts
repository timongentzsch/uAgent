// The person's own stylesheet (~/.uagent/config/style.css), which overrides
// the design tokens. It goes in after the app's own sheets. The agent edits
// it during a turn and a person can edit it any time, so it is read again
// when the host connects and whenever a turn starts or ends; the answer is
// revalidated, so an unchanged sheet costs a 304.
import { routes } from "../state/api.ts";

let sheet: HTMLStyleElement | undefined;

export async function refreshUserStyle() {
  let css: string;
  try {
    const response = await fetch(routes.userStyle, { cache: "no-cache" });
    if (!response.ok) return;
    css = await response.text();
  } catch {
    return;
  }
  if (sheet?.textContent === css) return;
  sheet ??= document.createElement("style");
  sheet.textContent = css;
  document.head.append(sheet);
}
