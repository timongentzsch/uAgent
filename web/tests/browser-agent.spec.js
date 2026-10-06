// What the agent's browser tool does to a page, in a real Chromium: the
// scripts and key events are taken from src/browser/runtime.cc as they are
// and sent the way the runtime sends them.
import { readFileSync } from "node:fs";
import { test, expect } from "@playwright/test";

const source = readFileSync(
  new URL("../../src/browser/runtime.cc", import.meta.url),
  "utf8",
);
const script = (name) =>
  source.match(new RegExp(`${name} = R"js\\(([\\s\\S]*?)\\)js";`))[1];
const pageScript = script("kPageScript");
const elementScript = script("kElementScript");
const keys = Object.fromEntries(
  [
    ...source
      .slice(source.indexOf("kKeys[]"))
      .matchAll(/\{"(\w+)", (\d+), "((?:\\.|[^"\\])*)"\}/g),
  ].map(([, name, code, text]) => [
    name,
    { code: Number(code), text: JSON.parse(`"${text}"`) },
  ]),
);

async function control(page) {
  const cdp = await page.context().newCDPSession(page);
  const run = async (expression) =>
    (await cdp.send("Runtime.evaluate", { expression, returnByValue: true }))
      .result.value;
  return {
    look: () => run(`${pageScript}(0,"",true)`),
    read: (offset, find) =>
      run(`${pageScript}(${offset},${JSON.stringify(find)},false)`),
    where: (number) => run(`${elementScript}(${number})`),
    // As Runtime::Act sends a key.
    press: async (name) => {
      const { code, text } = keys[name];
      const event = {
        type: text ? "keyDown" : "rawKeyDown",
        key: text === " " ? text : name,
        code: name,
        windowsVirtualKeyCode: code,
      };
      if (text) event.text = text;
      await cdp.send("Input.dispatchKeyEvent", event);
      delete event.text;
      await cdp.send("Input.dispatchKeyEvent", { ...event, type: "keyUp" });
    },
    replace: async (text) => {
      await cdp.send("Input.dispatchKeyEvent", {
        type: "rawKeyDown",
        commands: ["selectAll"],
      });
      await cdp.send("Input.insertText", { text });
    },
  };
}

test("a look numbers what can be acted on and never shows a field's value", async ({
  page,
}) => {
  await page.goto("about:blank");
  await page.setContent(`<title>Sign in</title>
    <label>Name <input id=name value="Ada Lovelace"></label>
    <input type=password value="hunter2" placeholder="Password">
    <label><input type=checkbox checked> Remember me</label>
    <select aria-label="Region"><option>North<option selected>South</select>
    <button>Go</button><button disabled>Off</button>
    <a href="/help">Help</a><a href="/hidden" style="display:none">Hidden</a>`);
  const seen = await (await control(page)).look();
  expect(seen.elements.split("\n")).toEqual([
    '[1] text "Name" (filled)',
    '[2] password "Password" (filled)',
    '[3] checkbox "Remember me" (checked)',
    '[4] select "Region" (South)',
    '[5] button "Go"',
    '[6] link "Help"',
  ]);
  expect(seen.title).toBe("Sign in");
  expect(JSON.stringify(seen)).not.toMatch(/hunter2|Ada Lovelace/);
});

test("an element is clicked where it is now, and not when it is gone or covered", async ({
  page,
}) => {
  await page.setViewportSize({ width: 800, height: 600 });
  await page.goto("about:blank");
  await page.setContent(`<html style="scroll-behavior:smooth"><body>
    <button id=near>Near</button><div style="height:3000px"></div>
    <button id=far>Far</button><button id=gone>Gone</button>
    <button id=under>Under</button></body></html>`);
  const agent = await control(page);
  await agent.look();
  // Far below: brought into view at once, whatever the page's scrolling.
  const far = await agent.where(2);
  expect(far.y).toBeGreaterThan(0);
  expect(far.y).toBeLessThan(600);
  expect(
    await page.evaluate(({ x, y }) => document.elementFromPoint(x, y).id, far),
  ).toBe("far");
  await page.evaluate(() => {
    document.getElementById("gone").style.display = "none";
    const cover = document.createElement("div");
    cover.style.cssText = "position:fixed;inset:0;background:#fff";
    document.body.append(cover);
  });
  expect(await agent.where(3)).toBeNull();
  expect(await agent.where(4)).toBeNull();
  expect(await agent.where(99)).toBeNull();
});

test("reading keeps links whole and what was searched for in view", async ({
  page,
}) => {
  await page.goto("about:blank");
  const long = "x".repeat(400);
  await page.setContent(`<base href="https://example.com/">
    <a href="/item?id=1">First</a><a href="/item?id=2">Second</a>
    <a href="/item?id=3">${"n".repeat(90)} needle</a>
    <svg><a href="/drawn"><text>Drawn</text></a></svg>
    <p>${long} needle ${long}</p><p>plain</p>
    ${Array.from({ length: 250 }, (_, i) => `<p>row ${i} needle</p>`).join("")}`);
  const agent = await control(page);
  const all = await agent.read(0, "");
  expect(all.links.split("\n").slice(0, 2)).toEqual([
    "First -> https://example.com/item?id=1",
    "Second -> https://example.com/item?id=2",
  ]);
  const found = await agent.read(0, "NEEDLE");
  expect(found.matches).toBe(252);
  const lines = found.text.split("\n");
  expect(lines).toHaveLength(200);
  // The match is far into its line and still in what comes back.
  expect(lines[0]).toContain("needle");
  expect(lines[0].length).toBeLessThanOrEqual(300);
  // So is the link whose name is longer than what is shown of it.
  expect(found.links).toContain("https://example.com/item?id=3");
  const rest = await agent.read(200, "needle");
  expect(rest.text.split("\n")).toHaveLength(52);
  expect(rest.text.split("\n").at(-1)).toBe("row 249 needle");
  expect((await agent.read(5, "")).links).toBe("");
});

test("a bot wall is named to a reader as to a look", async ({ page }) => {
  await page.goto("about:blank");
  await page.setContent("<title>Just a moment...</title><p>Checking</p>");
  const agent = await control(page);
  expect((await agent.look()).block.kind).toBe("bot_check");
  expect((await agent.read(0, "anything")).block.kind).toBe("bot_check");
});

test("keys do what they do for a person", async ({ page }) => {
  await page.goto("about:blank");
  await page.setContent(`
    <form onsubmit="window.sent=this.q.value;return false">
      <input name=q value="hello world"><button>Go</button></form>
    <textarea id=t>one two</textarea><input type=checkbox id=c>
    <div contenteditable id=e>old</div>`);
  const agent = await control(page);
  const field = page.locator("input[name=q]");
  await field.focus();
  await agent.press("End");
  await agent.press("Backspace");
  await agent.press("Home");
  await agent.press("Delete");
  await agent.press("ArrowRight");
  await agent.press("Space");
  await expect(field).toHaveValue("e llo worl");
  await agent.replace("kestrel");
  await expect(field).toHaveValue("kestrel");
  await agent.press("Enter");
  expect(await page.evaluate(() => window.sent)).toBe("kestrel");
  await agent.press("Tab");
  expect(await page.evaluate(() => document.activeElement.tagName)).toBe(
    "BUTTON",
  );
  await page.locator("#t").focus();
  await agent.press("Enter");
  expect(await page.locator("#t").inputValue()).toContain("\n");
  await page.locator("#c").focus();
  await agent.press("Space");
  await expect(page.locator("#c")).toBeChecked();
  await page.locator("#e").focus();
  await agent.replace("new");
  await expect(page.locator("#e")).toHaveText("new");
});
