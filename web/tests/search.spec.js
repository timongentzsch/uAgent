// Search: words in any conversation from Mod+K, and a find bar that steps
// through every occurrence in the open one.
import { connected, test, expect, saveHistory } from "./fixtures.js";

const mod = process.platform === "darwin" ? "Meta" : "Control";

// A conversation longer than one page, the words once near its start, twice
// near its end, and once in a command's output.
async function seed(page, host) {
  const messages = Array.from({ length: 160 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content:
      index === 2
        ? "Where did the quokka go?"
        : index === 151
          ? "A quokka, then another quokka."
          : `Ordinary message ${index}.`,
  }));
  const kinds = messages.map((message) => message.role);
  messages.push(
    {
      role: "assistant",
      content: "",
      tool_calls: [
        {
          id: "call-1",
          type: "function",
          function: { name: "run", arguments: '{"command":"ls"}' },
        },
      ],
    },
    // The message holds the model's hint too; search finds what its facts keep.
    {
      role: "tool",
      tool_call_id: "call-1",
      content: "found a quokka here\n[exit code 0]",
    },
  );
  kinds.push("assistant", "tool_result");
  const display = {
    ids: messages.map((_, index) => index + 1),
    next: messages.length + 1,
    facts: {
      "t-call-1": {
        name: "run",
        call_id: "call-1",
        state: "ok",
        result: { word: "1ms" },
        output: "found a quokka here",
      },
    },
  };
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
  await saveHistory(host, "needle.json", {
    id: "needle",
    title: "Needle",
    turns: 80,
    messages,
    kinds,
    display,
  });
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.getByRole("button", { name: /Needle/ })).toBeVisible();
}

const findBar = (page) =>
  page.getByRole("search", { name: "Find in this conversation" });
const marked = (page, name) =>
  page.evaluate((name) => CSS.highlights.has(name), name);

test("search finds words in another conversation and opens it at them", async ({
  page,
  host,
}) => {
  await seed(page, host);
  await page.keyboard.press(`${mod}+k`);
  const search = page.getByRole("dialog", { name: "Search" });
  await search
    .getByRole("combobox", {
      name: "Search conversations, messages, commands and settings",
    })
    .fill("quokka");
  const group = search.getByRole("group").filter({ hasText: "Needle" });
  await expect(group.getByRole("option").first()).toContainText("Needle");
  await expect(group.getByRole("option").first()).toContainText("3 matches");
  const hit = group
    .getByRole("option")
    .filter({ hasText: "Where did the quokka go?" });
  await expect(hit.locator("mark")).toHaveText("quokka");
  await hit.click();
  const bar = findBar(page);
  await expect(bar.getByRole("searchbox")).toHaveValue("quokka");
  await expect(bar).toContainText("1 of 4");
  const row = page
    .locator(".transcript .message")
    .filter({ hasText: "Where did the quokka go?" });
  await expect(row).toBeInViewport();
  await expect.poll(() => marked(page, "find-current")).toBe(true);
});

test("the find bar steps through every occurrence and opens folded rows", async ({
  page,
  host,
}) => {
  await seed(page, host);
  await page.getByRole("button", { name: /Needle/ }).click();
  await expect(page.locator(".conversation-head h1")).toHaveText("Needle");
  await page
    .locator(".conversation-head")
    .getByRole("button", { name: "Conversation menu" })
    .click();
  await page.getByRole("menuitem", { name: "Find in conversation" }).click();
  const bar = findBar(page);
  const field = bar.getByRole("searchbox");
  await expect(field).toBeFocused();
  await field.fill("QUOKKA");
  await expect(bar).toContainText("4 of 4");
  await expect(page.getByText("found a quokka here")).toBeInViewport();
  await field.press("Shift+Enter");
  await expect(bar).toContainText("3 of 4");
  await expect(
    page.locator(".transcript .message").filter({ hasText: "then another" }),
  ).toBeInViewport();
  await field.press("Enter");
  await field.press("Enter");
  await expect(bar).toContainText("1 of 4");
  await expect(
    page.locator(".transcript .message").filter({ hasText: "Where did" }),
  ).toBeInViewport();
  await expect.poll(() => marked(page, "find")).toBe(true);
  await field.press("Escape");
  await expect(bar).toHaveCount(0);
  await expect.poll(() => marked(page, "find")).toBe(false);
  await page.keyboard.press(`${mod}+f`);
  await expect(findBar(page).getByRole("searchbox")).toBeFocused();
});

test("nothing found says so", async ({ page, host }) => {
  await seed(page, host);
  await page.getByRole("button", { name: /Needle/ }).click();
  await page.keyboard.press(`${mod}+f`);
  await findBar(page).getByRole("searchbox").fill("wombat");
  await expect(findBar(page)).toContainText("No matches");
  await expect(
    findBar(page).getByRole("button", { name: "Next match" }),
  ).toBeDisabled();
});

test("the find bar keeps focus on words the transcript does not show", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
  // The host counts the link's address; the message shows only its text.
  await saveHistory(host, "burrow.json", {
    id: "burrow",
    title: "Burrow",
    turns: 1,
    messages: [
      { role: "user", content: "See [the burrow](https://example.com/numbat)" },
      { role: "assistant", content: "Noted." },
    ],
  });
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Burrow/ }).click();
  await page.keyboard.press(`${mod}+f`);
  const field = findBar(page).getByRole("searchbox");
  await expect(field).toBeFocused();
  await field.pressSequentially("numbat");
  await expect(findBar(page)).toContainText("1 of 1");
  await expect(page.getByRole("menu")).toHaveCount(0);
  await expect(field).toBeFocused();
});

test("search is a centered panel that groups, marks and steps back", async ({
  page,
  host,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
  const far = `${"Ordinary words go on for a while. ".repeat(8)}a wombat`;
  await saveHistory(host, "far.json", {
    id: "far",
    title: "Far",
    turns: 6,
    messages: Array.from({ length: 12 }, (_, index) => ({
      role: index % 2 ? "assistant" : "user",
      content: index % 2 ? `${far} ${index}` : `Question ${index}`,
    })),
  });
  await saveHistory(host, "elsewhere.json", {
    id: "elsewhere",
    title: "Elsewhere",
    turns: 1,
    cwd: host.home,
    messages: [
      { role: "user", content: "a wombat elsewhere" },
      { role: "assistant", content: "noted" },
    ],
  });
  // A title as long as the message it came from holds the letters of most
  // words somewhere, in order: it matches only a word it has.
  const scattered = "Write one more brief abstract today, then stop";
  await saveHistory(host, "scattered.json", {
    id: "scattered",
    title: scattered,
    turns: 1,
    messages: [
      { role: "user", content: scattered },
      { role: "assistant", content: "noted" },
    ],
  });
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  // The open conversation's place leads.
  await page.getByRole("button", { name: /Elsewhere/ }).click();
  await page.keyboard.press(`${mod}+k`);
  const search = page.getByRole("dialog", { name: "Search" });
  const box = await search.boundingBox();
  expect(Math.abs(box.x + box.width / 2 - 640)).toBeLessThan(4);
  expect(box.y).toBeLessThan(800 / 3);
  const field = search.getByRole("combobox");
  await field.fill("wombat");
  const groups = search.getByRole("group");
  await expect(groups.first()).toContainText("Elsewhere");
  await expect(
    search.getByRole("option", { name: /Write one more/ }),
  ).toHaveCount(0);
  const hits = search.locator(".spotlight-snippet");
  await expect(hits).toHaveCount(4);
  for (const hit of await hits.all())
    await expect(hit.locator("mark")).toHaveText("wombat");
  await expect(search).toContainText("3 more in this conversation");
  await field.press("ArrowDown");
  await expect(search.getByLabel("Preview").locator("mark")).not.toHaveCount(0);
  await field.fill(">shortcut");
  await expect(search.getByRole("group").first()).toContainText(
    "Actions & commands",
  );
  await expect(search.locator(".spotlight-snippet")).toHaveCount(0);
  // A command's description matches by whole words, not scattered letters.
  const permissions = search.getByRole("option", { name: /^\/permissions/ });
  await field.fill(">wombat");
  await expect(search.getByText("No command matches.")).toBeVisible();
  await field.fill(">approval");
  await expect(permissions).toHaveCount(1);
  await field.press("Escape");
  await expect(field).toHaveValue("");
  await expect(search).toBeVisible();
  await field.press("Escape");
  await expect(search).toHaveCount(0);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.keyboard.press(`${mod}+k`);
  await search.getByRole("combobox").fill("wombat");
  await expect(search.locator(".spotlight-snippet").first()).toBeVisible();
  await search.getByRole("combobox").press("ArrowDown");
  await expect(search.getByLabel("Preview")).toHaveCount(0);
});
