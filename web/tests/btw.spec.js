import { test, expect, online } from "./fixtures.js";

test.beforeEach(({ command, session }) =>
  command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  }),
);

for (const width of [320, 1280]) {
  test(`btw tools and follow-ups stay beside the parent (${width})`, async ({
    page,
    session,
    request,
  }, testInfo) => {
    await page.setViewportSize({ width, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const prompt = page.getByLabel("Message or guidance");
    await prompt.fill("/btw Side tool probe");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    const side = page.getByRole("complementary", { name: "Side conversation" });
    await expect(
      side.getByRole("button", { name: "Allow once", exact: true }),
    ).toBeVisible();
    await side.getByRole("button", { name: "Allow once", exact: true }).click();
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await expect(page.locator(".transcript")).not.toContainText(
      "SIDE-TOOL-COMPLETE",
    );
    await page.reload();
    await online(page);
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await side
      .getByLabel("Follow up on side conversation")
      .fill("Side followup probe");
    if (width === 1280)
      await side.getByLabel("Follow up on side conversation").press("Enter");
    else
      await side
        .getByRole("button", { name: "Send follow-up", exact: true })
        .click();
    await expect(side).toContainText("SIDE-FOLLOWUP-COMPLETE");
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    // Repeating /btw in the main composer follows up only on the side.
    // The local command must not fall through to the parent's submit path.
    await expect(
      side.locator("header").getByText("Ready", { exact: true }),
    ).toBeVisible();
    const repeated = [];
    page.on("request", (request) => {
      if (request.url().endsWith("/api/command")) {
        const body = request.postDataJSON();
        if (body.text?.includes("Side followup probe")) repeated.push(body);
      }
    });
    await prompt.fill("/btw Side followup probe");
    const receipt = page.waitForResponse(
      (response) =>
        response.url().endsWith("/api/command") &&
        response.request().postDataJSON().text === "Side followup probe",
    );
    await page.getByRole("button", { name: "Send", exact: true }).click();
    await receipt;
    await expect(prompt).toHaveValue("");
    // Wait for the repeated message to enter the side's canonical history,
    // rather than checking before a fall-through parent request can dispatch.
    await expect
      .poll(async () => {
        const snapshot = await (
          await request.get(`/api/sessions/${repeated[0].session_id}`)
        ).json();
        return snapshot.state.view.blocks.filter(
          (block) =>
            block.kind === "user" && block.text === "Side followup probe",
        ).length;
      })
      .toBe(2);
    expect(repeated).toHaveLength(1);
    expect(repeated[0].session_id).not.toBe(session.id);
    expect(repeated[0].text).toBe("Side followup probe");
    await expect(page).toHaveURL(new RegExp(`session=${session.id}$`));
    const layout = await side.evaluate((node) => ({
      overflow: document.documentElement.scrollWidth > innerWidth,
      width: node.getBoundingClientRect().width,
      height: node.getBoundingClientRect().height,
      button: node
        .querySelector('button[type="submit"]')
        .getBoundingClientRect().height,
    }));
    expect(layout.overflow).toBe(false);
    expect(layout.width).toBeLessThanOrEqual(width);
    expect(layout.height).toBeLessThan(500);
    if (width === 320) expect(layout.button).toBeGreaterThanOrEqual(44);
    await page.screenshot({ path: testInfo.outputPath("btw.png") });
    await side.getByRole("button", { name: "Open side conversation" }).click();
    await expect(page).not.toHaveURL(new RegExp(`session=${session.id}$`));
    await expect(page.locator(".transcript")).toContainText(
      "SIDE-FOLLOWUP-COMPLETE",
    );
  });
}
