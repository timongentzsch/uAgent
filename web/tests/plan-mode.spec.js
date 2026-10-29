import { test, expect, online, selectProbeModel } from "./fixtures.js";

for (const width of [1200, 390]) {
  test(`a plan is put to you with its file and three choices (${width}px)`, async ({
    page,
    session,
    command,
  }, testInfo) => {
    await page.setViewportSize({ width, height: 800 });
    await selectProbeModel(command, session);
    await command("permissions", {
      session_id: session.id,
      generation: session.generation,
      mode: "plan",
    });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const control = page.getByRole("button", { name: "Approval" });
    await expect(control).toContainText("Plan");
    const prompt = page.getByLabel("Message or guidance");
    await prompt.fill("Plan probe");
    await prompt.press("Enter");

    const decision = page.getByRole("region", { name: "Pending decision" });
    await expect(decision.getByText("Approve the plan?")).toBeVisible();
    // The plan opens from the decision, and stays in the conversation.
    await expect(
      decision.getByRole("link", { name: /plan\.md/ }),
    ).toBeVisible();
    await expect(page.locator(".transcript")).toContainText("Shared the plan");
    for (const name of [
      "Approve and run in auto",
      "Approve and ask before changes",
      "Keep planning",
    ]) {
      await expect(decision.getByRole("button", { name })).toBeVisible();
    }
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth > window.innerWidth,
      ),
    ).toBe(false);
    await page.screenshot({ path: testInfo.outputPath("plan-decision.png") });

    // Keep planning sends feedback; the next turn puts the plan again.
    await decision.getByRole("button", { name: "Keep planning" }).click();
    await decision.getByLabel("Guidance").fill("Add tests too");
    await decision.getByRole("button", { name: "Send guidance" }).click();
    await expect(page.locator(".transcript")).toContainText("Add tests too");
    await expect(control).toContainText("Plan");
    await decision
      .getByRole("button", { name: "Approve and ask before changes" })
      .click();
    await expect(decision).toHaveCount(0);
    await expect(page.locator(".transcript")).toContainText(
      "The plan is approved; carry it out.",
    );
    await expect(control).toContainText("Ask");
  });
}
