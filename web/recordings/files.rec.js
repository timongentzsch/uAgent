// Capture the conversation and slide deck using the release UI.
import {
  artboard,
  expect,
  online,
  saveHistory,
  test,
  viewerOf,
  writeFiles,
} from "../tests/fixtures.js";

test.use({
  viewport: { width: 1360, height: 588 },
  deviceScaleFactor: 1.5,
  colorScheme: "light",
  video: "off",
});

const deck = "decks/stockroom";
const first = deck + "/01-reservations.html";
const slide = (title, body) =>
  artboard(title, body, "800x500 slide").replace(
    "</head>",
    '<link rel="stylesheet" href="style.css"></head>',
  );
const css = `
* { box-sizing: border-box }
html, body { margin: 0; width: 800px; height: 500px; background: #f4f4e9; color: #142b22 }
body { padding: 30px; font: 20px/1.25 Arial,sans-serif }
header { font-size: 15px; font-weight: 700; letter-spacing: 2px; color: #234b3d }
h1 { margin: 17px 0 6px; font-size: 51px; letter-spacing: -2.6px; line-height: 1.02 }
p { margin: 0 0 20px }
.order { display: grid; grid-template-columns: 154px 1fr 75px 95px; align-items: center; gap: 10px; min-height: 80px; border-top: 1px solid #809789 }
.order small { display: block; font-size: 17px; margin-top: 2px }
.track { display: grid; grid-template-columns: repeat(8,1fr); gap: 5px }
.track i { height: 34px; background: #dee8dd }
.track .held, .key.held { background: #144e45 }
.result { text-align: right; font-weight: 700; color: #144e45 }
.declined { color: #994323 }
footer { display: flex; gap: 18px; align-items: center; padding-top: 18px; border-top: 1px solid #809789; font-size: 17px }
.key { display: inline-block; width: 13px; height: 13px; margin-right: 5px; background: #dee8dd }
pre { font: 25px/1.4 monospace; background: #183d33; color: #f4f4e9; padding: 24px; margin: 22px 0 }
`;

function order(name, requested, held, result) {
  const blocks = Array.from({ length: 8 }, (_, i) =>
    i < held ? '<i class="held"></i>' : "<i></i>",
  ).join("");
  return `<section class="order"><div><b>Order ${name}</b><small>${requested} requested</small></div><div class="track">${blocks}</div><span>${8 - held} free</span><span class="result ${result === "Declined" ? "declined" : ""}">${result}</span></section>`;
}

const sql =
  "UPDATE stock\nSET reserved = reserved + :qty\nWHERE sku = :sku\n  AND on_hand - reserved >= :qty;";
test("Stock reservation workspace", async ({ page, host, request }) => {
  await writeFiles(host.project, {
    [deck + "/style.css"]: css,
    [first]: slide(
      "Reserving the last units",
      `<header>STOCKROOM / INVENTORY</header><h1>Reserving the last units</h1><p>Eight units on hand. Three orders, in arrival order.</p>
      ${order("A", 4, 4, "Held")}${order("B", 2, 6, "Held")}${order("C", 3, 6, "Declined")}
      <footer><span><i class="key held"></i>Reserved</span><span><i class="key"></i>Available</span></footer><aside>A declined order leaves the quantity unchanged.</aside>`,
    ),
    [deck + "/02-atomic-update.html"]: slide(
      "Guard the write",
      `<header>STOCKROOM / CONCURRENCY</header><h1>Guard the write</h1><p>Checking inventory and reserving it in one update prevents overselling.</p><pre>${sql.replaceAll(">", "&gt;")}</pre><footer>One changed row confirms the reservation. Zero rows declines the order.</footer>`,
    ),
  });
  const id = crypto.randomUUID();
  const title = "Stock reservation review";
  await saveHistory(host, `stockroom-${id}.json`, {
    id,
    title,
    turns: 1,
    messages: [
      {
        role: "user",
        content:
          "We have eight units left. What happens if three orders request four, two and three units? Show me where the last order fails.",
      },
      {
        role: "assistant",
        content: `**The third order is declined.** The first two orders reserve six units, leaving two available.\n\nGuard the write itself so two checkouts cannot claim the same units.\n\n\`\`\`sql\n${sql}\n\`\`\`\n\nOrder C updates zero rows. The six units already held stay reserved.\n\n[Open the reservation deck](${first})`,
      },
    ],
  });
  let session;
  await expect
    .poll(async () => {
      const data = await (await request.get("/api/sessions?refresh=1")).json();
      session = data.sessions.find((s) => s.title === title);
      return Boolean(session);
    })
    .toBe(true);
  await page.goto(`/#session=${session.id}&file=${first}`);
  await online(page);
  const files = page.getByRole("region", { name: "Files" });
  const { stage, count } = viewerOf(files);
  await expect(count).toHaveText("1 / 2");
  await expect(
    stage.getByRole("heading", { name: "Reserving the last units" }),
  ).toBeVisible();
  await expect(
    files.getByRole("button", { name: "Edit", exact: true }),
  ).toBeVisible();
  await files.getByRole("button", { name: "Next slide" }).click();
  await expect(
    stage.getByRole("heading", { name: "Guard the write" }),
  ).toBeVisible();
  await files.getByRole("button", { name: "Previous slide" }).click();
  await expect(count).toHaveText("1 / 2");
  await expect(
    page.getByText("The third order is declined.", { exact: true }),
  ).toBeVisible();
  await files.getByRole("button", { name: "Legend", exact: true }).click();
  await page.mouse.move(0, 0);
  await page.evaluate(() => document.fonts.ready);
  await page.locator(".shell > .conversation").screenshot({
    path: "../docs/images/workspace.png",
    animations: "disabled",
  });
});
