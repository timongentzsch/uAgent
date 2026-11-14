// InputRow: a field shares its row with controls only while both fit.
import { test, expect, online } from "./fixtures.js";

test("a field keeps its room: wide controls take the next row", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  // The shared styles, on a row of our own: a field and one control.
  const measure = (width, label) =>
    page.evaluate(
      ({ width, label }) => {
        const row = document.createElement("div");
        row.className = "input-row";
        row.style.width = `${width}px`;
        row.innerHTML =
          '<span class="text-control"><input /></span><button type="button"></button>';
        row.lastChild.textContent = label;
        document.body.append(row);
        const [field, control, box] = [row.firstChild, row.lastChild, row].map(
          (node) => node.getBoundingClientRect(),
        );
        row.remove();
        return {
          beside: control.top < field.bottom && control.left >= field.right,
          field: Math.round(field.width),
          inside: control.right <= box.right + 1,
        };
      },
      { width, label },
    );
  const wide = await measure(600, "Show the file");
  expect(wide).toMatchObject({ beside: true, inside: true });
  expect(wide.field).toBeGreaterThan(400);
  // The same control with a name that leaves the field too little.
  const long = await measure(
    600,
    "provider/a-model-with-a-very-long-name:high and more",
  );
  expect(long).toEqual({ beside: false, field: 600, inside: true });
  // A narrow box: the field takes all of it before it gives any up.
  expect(await measure(200, "Show the file")).toEqual({
    beside: false,
    field: 200,
    inside: true,
  });
  // A name wider than the box is held inside it.
  expect((await measure(200, "x".repeat(80))).inside).toBe(true);
});
