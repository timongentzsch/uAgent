import { test, expect } from "./fixtures.js";

test("pinned downloads and drafts are cleared on logout", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await page
    .getByRole("complementary")
    .getByRole("button", { name: "Conversation menu", exact: true })
    .click();
  await page
    .getByRole("menuitem", { name: "Keep offline", exact: true })
    .click();
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page.getByText("Offline storage", { exact: true }).click();
  await expect(page.getByRole("dialog")).toContainText("1 saved conversation");
  await page.getByText("Paired devices", { exact: true }).click();
  await page
    .getByRole("button", { name: "Log out this device", exact: true })
    .click();
  await expect
    .poll(() =>
      page.evaluate(
        async () =>
          (await indexedDB.databases()).filter((db) =>
            db.name.startsWith("uagent-offline-"),
          ).length,
      ),
    )
    .toBe(0);
  expect(
    await page.evaluate(() => localStorage.getItem("uagent-offline-device")),
  ).toBeNull();
});

test("turn summaries and diagrams survive a cold offline launch without sending drafts", async ({
  page,
  context,
  session,
  request,
}) => {
  await page.goto(`/#session=${session.id}`);
  await page
    .getByRole("button", { name: "Model and effort", exact: true })
    .click();
  await page
    .getByLabel("Model", { exact: true })
    .selectOption({ label: "mock/model-b" });
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await page.getByLabel("Message or guidance").fill("Diagram probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Turn statistics", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("img", { name: "Mermaid diagram", exact: true }),
  ).toBeVisible();
  await expect
    .poll(() =>
      page
        .getByRole("img", { name: "Mermaid diagram", exact: true })
        .evaluate((image) => image.naturalWidth),
    )
    .toBeGreaterThan(0);
  await page
    .getByRole("button", { name: "Expand diagram", exact: true })
    .click();
  await expect(page.getByRole("dialog")).toBeVisible();
  await page
    .getByRole("button", { name: "Close diagram", exact: true })
    .click();
  await page
    .getByRole("button", { name: "Turn statistics", exact: true })
    .click();
  await expect(page.getByRole("dialog")).toContainText("120");
  await page.getByRole("button", { name: "Session", exact: true }).click();
  await expect(page.getByRole("dialog")).toContainText("Model calls");
  await page
    .getByRole("dialog")
    .getByRole("button", { name: /^Close/ })
    .click();
  await page
    .getByLabel("Message or guidance")
    .fill("Offline draft must stay unsent");
  await expect
    .poll(() =>
      page.evaluate(async () => {
        const name = `uagent-offline-${localStorage.getItem("uagent-offline-device")}`;
        return new Promise((resolve) => {
          const op = indexedDB.open(name);
          op.onsuccess = () => {
            const tx = op.result.transaction("meta");
            const value = tx.objectStore("meta").get("drafts");
            value.onsuccess = () =>
              resolve(
                JSON.stringify(value.result).includes(
                  "Offline draft must stay unsent",
                ),
              );
            tx.oncomplete = () => op.result.close();
          };
        });
      }),
    )
    .toBe(true);
  await page.evaluate(async () => {
    await navigator.serviceWorker.ready;
  });
  await page.reload();
  await expect
    .poll(() => page.evaluate(() => !!navigator.serviceWorker.controller))
    .toBe(true);
  await context.setOffline(true);
  await page.reload();
  await expect(
    page.getByText("Verified response", { exact: true }),
  ).toBeVisible();
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Offline draft must stay unsent",
  );
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeDisabled();
  await expect(
    page.getByRole("img", { name: "Mermaid diagram", exact: true }),
  ).toBeVisible();
  await page.screenshot({
    path: test.info().outputPath("offline-conversation.png"),
    fullPage: true,
  });
  await context.setOffline(false);
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeEnabled();
  const snapshot = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  expect(snapshot.state.statistics.recorded_turns).toBe(1);
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Offline draft must stay unsent",
  );
});
