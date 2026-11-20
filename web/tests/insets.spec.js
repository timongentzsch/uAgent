// At a desk's width: a control that stands in a corner of its box is as far
// from the box's side as from its end.
import {
  test,
  expect,
  headerAction,
  openFiles,
  openSheet,
  paper,
  selectProbeModel,
  sheet,
  slide,
  writeFiles,
} from "./fixtures.js";
import { uneven } from "./insets.js";

test.use({ viewport: { width: 1440, height: 900 }, places: true });

const even = async (page) => expect(await uneven(page)).toEqual([]);
const listed = (page, name) =>
  page.locator("aside.sidebar").getByRole("button", { name, exact: true });

test("a conversation, its list, its input and Settings", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session);
  await page.goto(`/#session=${session.id}`);
  const input = page.getByLabel("Message or guidance");
  await expect(input).toBeVisible();
  await even(page);
  // A draft of one line shares its row with what it is sent with.
  await input.fill("Native tool probe");
  await even(page);
  await headerAction(page, "Settings");
  await expect(
    page.getByRole("dialog", { name: "Settings", exact: true }),
  ).toBeVisible();
  await even(page);
});

test("the Library, and Scheduled with a new task", async ({
  page,
  host,
  session,
}) => {
  await writeFiles(host.project, {
    "docs/memo.html":
      '<!doctype html><meta name="print" content="flow a4"><title>Memo</title>',
  });
  await page.goto(`/#session=${session.id}`);
  await listed(page, "Library").click();
  await expect(
    page.locator(".library-row:not(.placeholder *)").first(),
  ).toBeVisible();
  await even(page);
  await listed(page, "Scheduled").click();
  await page.getByRole("button", { name: "New task", exact: true }).click();
  await expect(page.getByLabel("Name", { exact: true })).toBeVisible();
  await even(page);
});

test("a deck beside the conversation, with Edit off and on", async ({
  page,
  session,
  host,
}) => {
  await writeFiles(host.project, {
    "deck/01-cover.html": slide("Cover", "Say hello."),
    "deck/02-plan.html": slide("Plan", "Three steps."),
  });
  const files = await openFiles(page, session, "deck/");
  await expect(files.locator(".page-stage iframe").first()).toBeVisible();
  await even(page);
  await files.getByRole("button", { name: "Edit" }).click();
  await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
  await even(page);
});

test("a document beside the conversation", async ({ page, session, host }) => {
  await writeFiles(host.project, {
    "memo.html": paper("Memo", "<h1>Memo</h1><p>Text.</p>"),
  });
  const files = await openFiles(page, session, "memo.html");
  await expect(files.locator(".page-stage iframe").first()).toBeVisible();
  await even(page);
});

test("a sheet beside the conversation", async ({ page, session, host }) => {
  await openSheet(
    page,
    session,
    host.project,
    sheet("Plan", { Tasks: ["<th>Task<th>Cost", "<td>Write<td>1200"] }),
  );
  await even(page);
});
