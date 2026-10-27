// Search: words in any conversation from Mod+K, and a find bar that steps
// through every occurrence in the open one.
import { test, expect, saveHistory } from "./fixtures.js";

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
    { role: "tool", tool_call_id: "call-1", content: "found a quokka here" },
  );
  kinds.push("assistant", "tool_result");
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await saveHistory(host, "needle.json", {
    id: "needle",
    title: "Needle",
    turns: 80,
    messages,
    kinds,
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
  // Grouped by place, then by conversation: its name once, its hits under
  // it, each with the words marked.
  const group = search.getByRole("group").filter({ hasText: "Needle" });
  await expect(group.getByRole("option").first()).toContainText("Needle");
  await expect(group.getByRole("option").first()).toContainText("3 matches");
  const hit = group
    .getByRole("option")
    .filter({ hasText: "Where did the quokka go?" });
  await expect(hit.locator("mark")).toHaveText("quokka");
  await hit.click();
  // The row is pages back: they load until it is there, in view.
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
  // It starts on the newest: the command's output, opened to show it.
  await expect(bar).toContainText("4 of 4");
  await expect(page.getByText("found a quokka here")).toBeInViewport();
  await field.press("Shift+Enter");
  await expect(bar).toContainText("3 of 4");
  await expect(
    page.locator(".transcript .message").filter({ hasText: "then another" }),
  ).toBeInViewport();
  await field.press("Enter");
  await field.press("Enter");
  // Past the last it wraps to the first, a page back.
  await expect(bar).toContainText("1 of 4");
  await expect(
    page.locator(".transcript .message").filter({ hasText: "Where did" }),
  ).toBeInViewport();
  await expect.poll(() => marked(page, "find")).toBe(true);
  await field.press("Escape");
  await expect(bar).toHaveCount(0);
  await expect.poll(() => marked(page, "find")).toBe(false);
  // Mod+F opens it again over the conversation.
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

// Spotlight's own shape and keys: centered high, every hit marked, places in
// their order, more under a conversation, ">" for commands, Esc stepping back.
test("search is a centered panel that groups, marks and steps back", async ({
  page,
  host,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
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
  // Every hit shows its mark, however far into its message the words are.
  const hits = search.locator(".spotlight-snippet");
  await expect(hits).toHaveCount(4);
  for (const hit of await hits.all())
    await expect(hit.locator("mark")).toHaveText("wombat");
  await expect(search).toContainText("3 more in this conversation");
  // The selected hit (under the first conversation) shows its neighbourhood
  // beside the list.
  await field.press("ArrowDown");
  await expect(search.getByLabel("Preview").locator("mark")).not.toHaveCount(0);
  // ">" lists only actions, commands and settings.
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
  // Esc clears first, then closes.
  await field.press("Escape");
  await expect(field).toHaveValue("");
  await expect(search).toBeVisible();
  await field.press("Escape");
  await expect(search).toHaveCount(0);
  // A phone has no preview.
  await page.setViewportSize({ width: 390, height: 844 });
  await page.keyboard.press(`${mod}+k`);
  await search.getByRole("combobox").fill("wombat");
  await expect(search.locator(".spotlight-snippet").first()).toBeVisible();
  await search.getByRole("combobox").press("ArrowDown");
  await expect(search.getByLabel("Preview")).toHaveCount(0);
});
