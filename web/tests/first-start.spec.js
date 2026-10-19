// A conversation with no model chosen opens the model picker at once, and the
// first choice is kept for the conversations after it.
import { test, expect } from "./fixtures.js";

test.use({ noModel: true });

test("a conversation with no model starts at the picker", async ({
  page,
  host,
  request,
  command,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  const picker = page.getByRole("dialog", {
    name: "Model and effort",
    exact: true,
  });
  await expect(picker).toBeVisible();
  await picker
    .getByRole("combobox", { name: "Model", exact: true })
    .fill("model-b");
  await page.getByRole("option").filter({ hasText: "mock/model-b" }).click();
  await picker.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(picker).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: "Model and effort", exact: true }),
  ).toContainText("model-b");

  // A conversation started after it already has the model.
  let { session: next } = await command("create", { cwd: host.project });
  ({ session: next } = await command("activate", { session_id: next.id }));
  await expect
    .poll(async () => {
      const response = await request.get(`/api/sessions/${next.id}`);
      return (await response.json()).state?.route;
    })
    .toContain("model-b");
});
