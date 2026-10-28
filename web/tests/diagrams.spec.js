// Every Mermaid diagram type draws as an image. The bundle leaves out the
// ELK layout engine, so a diagram that asks for it is arranged by dagre.
import { test, expect } from "./fixtures.js";

const DIAGRAMS = {
  flowchart:
    "flowchart TD\n  A[Start] --> B{Choice}\n  B --> C[Yes]\n  B --> D[No]",
  sequence: "sequenceDiagram\n  Ada->>Bob: Hello\n  Bob-->>Ada: Hi",
  class: "classDiagram\n  Animal <|-- Dog\n  Animal : +name",
  state: "stateDiagram-v2\n  [*] --> Idle\n  Idle --> Busy\n  Busy --> [*]",
  gantt:
    "gantt\n  dateFormat YYYY-MM-DD\n  section Work\n  Build :a1, 2026-01-01, 3d",
  elk: "---\nconfig:\n  layout: elk\n---\nflowchart LR\n  A --> B --> C",
};

test("each diagram type renders, and elk falls back to dagre", async ({
  page,
  session,
  request,
}) => {
  const snapshot = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  snapshot.state.view.blocks = [
    {
      id: "diagrams",
      parent: null,
      kind: "message",
      role: "assistant",
      body: {
        kind: "markdown",
        text: Object.values(DIAGRAMS)
          .map((source) => `\`\`\`mermaid\n${source}\n\`\`\``)
          .join("\n\n"),
      },
    },
  ];
  await page.route(`**/api/sessions/${session.id}`, (route) =>
    route.fulfill({ json: snapshot }),
  );
  await page.goto(`/#session=${session.id}`);
  const images = page.getByRole("img", { name: "Mermaid diagram" });
  await expect(images).toHaveCount(Object.keys(DIAGRAMS).length);
  for (const image of await images.all())
    await expect
      .poll(() => image.evaluate((node) => node.naturalWidth))
      .toBeGreaterThan(0);
  await expect(page.getByText("could not be rendered")).toHaveCount(0);
});
