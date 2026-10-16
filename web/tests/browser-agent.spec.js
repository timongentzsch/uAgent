// What the agent's browser tool does to a page, in a real Chromium: the
// scripts and key events are taken from src/browser/runtime_*.cc as they are
// and sent the way the runtime sends them.
import { readFileSync } from "node:fs";
import { test, expect } from "@playwright/test";

const source = ["runtime_act.cc", "runtime_page.cc"]
  .map((name) =>
    readFileSync(new URL(`../../src/browser/${name}`, import.meta.url), "utf8"),
  )
  .join("\n");
const script = (name) =>
  source.match(new RegExp(`${name} = R"js\\(([\\s\\S]*?)\\)js";`))[1];
// The page script is built from three pieces around the text limit it takes
// from browser.h, which this joins the way PageScript() does.
const pageTextChars = readFileSync(
  new URL("../../include/browser/browser.h", import.meta.url),
  "utf8",
).match(/kPageTextChars = (\d+)/)[1];
const pageScript = [
  ...source
    .match(/std::string PageScript\(\) \{[\s\S]*?\n\}\n/)[0]
    .matchAll(/R"js\(([\s\S]*?)\)js"/g),
]
  .map((piece) => piece[1])
  .join(pageTextChars);
const elementScript = script("kElementScript");
// The mask is one C++ string of adjacent literals.
const secretScript = readFileSync(
  new URL("../../src/browser/runtime_internal.h", import.meta.url),
  "utf8",
)
  .match(/kSecretScript =\s*((?:"(?:\\.|[^"\\])*"\s*)+);/)[1]
  .match(/"((?:\\.|[^"\\])*)"/g)
  .map((literal) => JSON.parse(literal))
  .join("");
const secretFieldScript = script("kSecretFieldScript");
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
    (
      await cdp.send("Runtime.evaluate", {
        expression,
        returnByValue: true,
        awaitPromise: true,
      })
    ).result.value;
  return {
    look: () => run(`${pageScript}(0,"",true)`),
    read: (offset, find) =>
      run(`${pageScript}(${offset},${JSON.stringify(find)},false)`),
    where: (number) => run(`${elementScript}(${number})`),
    secret: () => run(secretFieldScript),
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
    '[6] button "Off" (disabled)',
    '[7] link "Help"',
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
  expect(await agent.where(4)).toEqual({ covered: "div" });
  expect(await agent.where(99)).toBeNull();
});

test("a covered element names what covers it, and a moving one is clicked where it stops or not at all", async ({
  page,
}) => {
  await page.setViewportSize({ width: 800, height: 600 });
  await page.goto("about:blank");
  await page.setContent(`<button id=buy>Buy</button>
    <button id=slide style="position:relative;left:0;transition:left .1s">Slide</button>
    <style>@keyframes spin { to { transform: translateX(200px) } }</style>
    <button id=spin style="animation: spin 1s linear infinite">Spin</button>
    <div id=cookie-banner class="banner wide" style="position:fixed;inset:0">
      We use cookies. <button>Accept all</button></div>`);
  const agent = await control(page);
  await agent.look();
  expect(await agent.where(1)).toEqual({
    covered: 'div#cookie-banner.banner "We use cookies. Accept all"',
  });
  await page.evaluate(() => document.getElementById("cookie-banner").remove());
  await page.evaluate(
    () => (document.getElementById("slide").style.left = "300px"),
  );
  const slide = await agent.where(2);
  // Where it stopped, not where it was on the way.
  expect(slide).toEqual(
    await page.evaluate(() => {
      const r = document.getElementById("slide").getBoundingClientRect();
      return {
        x: Math.round(r.x + r.width / 2),
        y: Math.round(r.y + r.height / 2),
      };
    }),
  );
  expect(await agent.where(3)).toEqual({ moving: true });
});

test("a password field is known as one, also once a reveal toggle shows it", async ({
  page,
}) => {
  await page.goto("about:blank");
  await page.setContent(`<input id=name><input id=pw type=password>
    <input id=fresh autocomplete=new-password><input id=code autocomplete=one-time-code>`);
  const agent = await control(page);
  const focused = async (id) => {
    await page.focus(`#${id}`);
    return agent.secret();
  };
  expect(await focused("name")).toBe(false);
  expect(await focused("pw")).toBe(true);
  expect(await focused("fresh")).toBe(true);
  expect(await focused("code")).toBe(false);
  // As kSecretScript tags it before a reveal toggle turns it into text.
  await page.evaluate(() => {
    const field = document.getElementById("pw");
    field.setAttribute("data-uagent-secret", "");
    field.type = "text";
  });
  expect(await focused("pw")).toBe(true);
  // Typed text goes where focus really is: into a shadow root, or a frame
  // of the same site. Another site's frame cannot be told.
  await page.setContent(`<div id=host></div>
    <iframe id=same srcdoc="<input id=inner type=password>"></iframe>
    <iframe id=other src="data:text/html,<input id=inner>"></iframe>`);
  await page.evaluate(() => {
    const root = document.getElementById("host").attachShadow({ mode: "open" });
    root.innerHTML = "<input id=deep type=password>";
    root.getElementById("deep").focus();
  });
  expect(await agent.secret()).toBe(true);
  await page.frameLocator("#same").locator("#inner").focus();
  expect(await agent.secret()).toBe(true);
  await page.frameLocator("#other").locator("#inner").focus();
  expect(await agent.secret()).toBe("frame");
});

test("a look reaches into open shadow roots and frames of the site, and says how a control stands", async ({
  page,
}) => {
  await page.setViewportSize({ width: 800, height: 600 });
  await page.goto("about:blank");
  await page.setContent(`<div id=host></div>
    <button aria-expanded=false>Menu</button>
    <label>Email <input required></label>
    <iframe id=frame style="position:absolute;left:300px;top:200px;width:300px;height:200px;border:0"
      srcdoc="<button id=inner>Inside</button>"></iframe>`);
  await page.frameLocator("#frame").locator("#inner").waitFor();
  await page.evaluate(() => {
    const root = document.getElementById("host").attachShadow({ mode: "open" });
    root.innerHTML = "<button id=deep>Deep</button>";
  });
  const agent = await control(page);
  const seen = await agent.look();
  expect(seen.elements.split("\n")).toEqual([
    '[1] button "Deep"',
    '[2] button "Menu" (collapsed)',
    '[3] text "Email" (empty) (required)',
    '[4] button "Inside"',
  ]);
  // A click lands on the element itself, through its shadow root or frame.
  const deep = await agent.where(1);
  expect(
    await page.evaluate(
      ({ x, y }) =>
        document.elementFromPoint(x, y).shadowRoot.elementFromPoint(x, y).id,
      deep,
    ),
  ).toBe("deep");
  const inside = await agent.where(4);
  expect(inside.x).toBeGreaterThan(300);
  expect(inside.y).toBeGreaterThan(200);
  expect(
    await page.evaluate(({ x, y }) => {
      const frame = document.elementFromPoint(x, y);
      const box = frame.getBoundingClientRect();
      return frame.contentDocument.elementFromPoint(x - box.x, y - box.y).id;
    }, inside),
  ).toBe("inner");
});

test("a frame's border, padding and size count where a click lands", async ({
  page,
}) => {
  await page.setViewportSize({ width: 800, height: 600 });
  await page.goto("about:blank");
  await page.setContent(`
    <fieldset disabled><input aria-label="Locked"></fieldset>
    <iframe hidden srcdoc="<button>Hidden</button>"></iframe>
    <iframe id=padded style="position:absolute;left:100px;top:100px;width:200px;height:120px;border:7px solid;padding:13px"
      srcdoc="<button id=inner style='margin:0'>Padded</button>"></iframe>
    <iframe id=scaled style="position:absolute;left:400px;top:100px;width:200px;height:120px;border:0;transform:scale(2);transform-origin:0 0"
      srcdoc="<button>Scaled</button>"></iframe>`);
  await page.frameLocator("#padded").locator("#inner").waitFor();
  const agent = await control(page);
  const seen = await agent.look();
  expect(seen.elements.split("\n")).toEqual([
    '[1] text "Locked" (empty) (disabled)',
    '[2] button "Padded"',
    '[3] button "Scaled"',
  ]);
  const padded = await agent.where(2);
  expect(
    await page.evaluate(({ x, y }) => {
      const frame = document.getElementById("padded");
      const box = frame.getBoundingClientRect();
      return frame.contentDocument.elementFromPoint(
        x - box.x - 20,
        y - box.y - 20,
      ).id;
    }, padded),
  ).toBe("inner");
  expect(await agent.where(3)).toEqual({ transformed: true });
});

test("a password field is masked in an open shadow root too", async ({
  page,
}) => {
  await page.goto("about:blank");
  await page.setContent(`<div id=host></div>`);
  await page.evaluate(secretScript);
  await page.evaluate(() => {
    const root = document.getElementById("host").attachShadow({ mode: "open" });
    root.innerHTML = "<input id=pw type=password>";
  });
  // A look again before a picture finds roots made since, also inside a
  // root it knew.
  await page.evaluate(secretScript);
  await page.evaluate(() => {
    const outer = document.getElementById("host").shadowRoot;
    const inner = document.createElement("span");
    outer.append(inner);
    inner.attachShadow({ mode: "open" }).innerHTML =
      "<input id=late type=password>";
  });
  await page.evaluate(secretScript);
  expect(
    await page.evaluate(() =>
      getComputedStyle(
        document
          .getElementById("host")
          .shadowRoot.querySelector("span")
          .shadowRoot.getElementById("late"),
      ).getPropertyValue("-webkit-text-security"),
    ),
  ).toBe("disc");
  expect(
    await page.evaluate(() =>
      getComputedStyle(
        document.getElementById("host").shadowRoot.getElementById("pw"),
      ).getPropertyValue("-webkit-text-security"),
    ),
  ).toBe("disc");
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
