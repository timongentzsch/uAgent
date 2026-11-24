// docs/images/files.gif: Files beside a conversation. The README renders
// with its image, a search opens a C++ source with line numbers, and the
// pane is resized.
import {
  test,
  expect,
  headerAction,
  online,
  savedConversation,
  writeFiles,
} from "../tests/fixtures.js";

// Wide enough for the list and a file side by side; the GIF is scaled down.
const size = { width: 1280, height: 760 };
test.use({ viewport: size, video: "off" });

const pause = (page, ms = 900) => page.waitForTimeout(ms);
const type = (field, text) => field.pressSequentially(text, { delay: 90 });

test("Files", async ({ page, host, request }, testInfo) => {
  await writeFiles(host.project, {
    "README.md": [
      "# Stockroom",
      "",
      "Reserves stock for an order and releases it when the order is cancelled.",
      "",
      "![How a reservation moves through the stockroom](docs/flow.svg)",
      "",
      "## Build",
      "",
      "```sh",
      "cmake -B build && cmake --build build",
      "ctest --test-dir build",
      "```",
      "",
    ].join("\n"),
    "docs/flow.svg":
      '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 520 120" font-family="monospace" font-size="14">' +
      '<rect width="520" height="120" fill="#f4f4f1"/>' +
      ["Order", "Reserve", "Ship"]
        .map(
          (label, i) =>
            `<rect x="${30 + i * 170}" y="35" width="120" height="50" fill="#fff" stroke="#111"/>` +
            `<text x="${90 + i * 170}" y="65" text-anchor="middle">${label}</text>` +
            (i < 2
              ? `<path d="M${150 + i * 170} 60 h50" stroke="#111" marker-end="url(#a)"/>`
              : ""),
        )
        .join("") +
      '<defs><marker id="a" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0 0 L8 4 L0 8z"/></marker></defs></svg>',
    "src/inventory.cc": [
      '#include "inventory.h"',
      "",
      "#include <algorithm>",
      "",
      "namespace stockroom {",
      "",
      "// Holds `count` of `item` for `order`, or says how many are left.",
      "Reservation Inventory::Reserve(const Order& order, ItemId item, int count) {",
      "  Stock& stock = stock_.at(item);",
      "  const int free = stock.on_hand - stock.reserved;",
      "  if (count > free) return Reservation::Short(free);",
      "  stock.reserved += count;",
      "  held_[order.id].push_back({item, count});",
      "  return Reservation::Held(order.id, item, count);",
      "}",
      "",
      "void Inventory::Release(OrderId order) {",
      "  for (const Hold& hold : held_[order]) {",
      "    stock_.at(hold.item).reserved -= hold.count;",
      "  }",
      "  held_.erase(order);",
      "}",
      "",
      "}  // namespace stockroom",
      "",
    ].join("\n"),
    "src/inventory.h": "#pragma once\n",
  });
  const session = await savedConversation(
    host,
    request,
    "Where is stock reserved for an order?",
    "`Inventory::Reserve` in `src/inventory.cc` holds the items for an order, and `Release` gives them back when the order is cancelled. The README has the flow.",
  );
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await headerAction(page, "Files");
  const files = page.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "README.md", exact: true }).click();
  const flow = files.getByAltText(
    "How a reservation moves through the stockroom",
  );
  await flow.evaluate((image) => image.decode());
  await page.screencast.start({
    path: testInfo.outputPath("video.webm"),
    size,
  });
  await pause(page, 1800);
  const search = files.getByRole("combobox", { name: "Search files" });
  await search.click();
  await type(search, "inventory.cc");
  await pause(page, 700);
  await search.press("Enter");
  await expect(
    files.locator("pre, code, table").filter({ hasText: "Reserve" }).first(),
  ).toBeVisible();
  await pause(page, 1800);
  // The pane's edge, dragged wider and back.
  const edge = await page
    .locator(".side-pane > .resize-handle")
    .first()
    .boundingBox();
  if (edge) {
    const y = edge.y + edge.height / 2;
    await page.mouse.move(edge.x + edge.width / 2, y);
    await page.mouse.down();
    await page.mouse.move(edge.x - 160, y, { steps: 18 });
    await pause(page, 500);
    await page.mouse.move(edge.x + 60, y, { steps: 18 });
    await page.mouse.up();
  }
  await pause(page, 1200);
  await page.screencast.stop();
});
