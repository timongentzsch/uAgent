// An MCP server's request for input, answered in the conversation: the
// snapshot is served with the request in it and the reply recorded.
import { test, expect, servePending } from "./fixtures.js";
import { uneven } from "./insets.js";

const FORM = {
  id: "elicit-form",
  kind: "elicit",
  prompt: "Where should the report go?",
  elicitation: {
    server: "reports",
    mode: "form",
    message: "Where should the report go?",
    requestedSchema: {
      type: "object",
      required: ["email", "format"],
      properties: {
        email: {
          type: "string",
          format: "email",
          title: "Email",
          choices: [],
        },
        copies: {
          type: "integer",
          title: "Copies",
          minimum: 1,
          maximum: 5,
          default: 1,
          choices: [],
        },
        format: {
          type: "string",
          title: "Format",
          choices: [
            { value: "pdf", label: "PDF" },
            { value: "html", label: "Web page" },
          ],
        },
        note: {
          type: "string",
          title: "Note",
          description: "Printed above the report.",
          choices: [],
        },
        sections: {
          type: "array",
          title: "Sections",
          choices: [
            { value: "summary", label: "Summary" },
            { value: "costs", label: "Costs" },
          ],
        },
        urgent: { type: "boolean", title: "Urgent", choices: [] },
      },
    },
  },
};

const LINK = {
  id: "elicit-url",
  kind: "elicit",
  prompt: "Connect your calendar.",
  elicitation: {
    server: "calendar",
    mode: "url",
    message: "Connect your calendar.",
    url: "https://auth.example.com/connect?state=abc",
    host: "auth.example.com",
  },
};

const replies = (commands) =>
  commands.map((command) =>
    command.cancelled ? "cancelled" : JSON.parse(command.text),
  );

test("an MCP form is filled in, checked and answered", async ({
  page,
  session,
  request,
}) => {
  const commands = await servePending(page, request, session, FORM);
  await page.goto(`/#session=${session.id}`);
  const card = page.getByRole("region", { name: "Pending decision" });
  await expect(
    card.getByRole("heading", { name: "Needs your answer" }),
  ).toBeVisible();
  await expect(card).toContainText("reports asks: Where should the report go?");
  await expect(card.getByLabel("Copies (optional)")).toHaveValue("1");
  await expect(card).toContainText("Printed above the report.");
  expect(await uneven(page)).toEqual([]);
  // Every label starts on one edge and every control ends on one.
  const edges = await card.locator(".elicit-fields").evaluate((box) => {
    const sides = (selector, side) =>
      new Set(
        [...box.querySelectorAll(selector)].map((node) =>
          Math.round(node.getBoundingClientRect()[side]),
        ),
      ).size;
    return {
      labels: sides(".field-label, .row-label", "left"),
      controls: sides(".text-control, .ask-options, .switch", "right"),
    };
  });
  expect(edges).toEqual({ labels: 1, controls: 1 });

  const email = card.getByLabel("Email");
  await email.fill("not an address");
  await card.getByRole("button", { name: "Accept" }).click();
  expect(await email.evaluate((input) => input.validity.valid)).toBe(false);

  await email.fill("ada@example.com");
  await card.getByLabel("Format").selectOption("html");
  await card
    .getByRole("group", { name: "Sections (optional)" })
    .getByRole("checkbox", { name: "Costs" })
    .check();
  // An optional yes/no left untouched is not sent.
  await card.getByRole("button", { name: "Accept" }).click();
  await card.getByRole("switch", { name: "Urgent" }).check();
  await card.getByRole("button", { name: "Accept" }).click();
  await card.getByRole("button", { name: "Decline" }).click();
  await card.getByRole("button", { name: "Cancel" }).click();
  await expect
    .poll(() => replies(commands))
    .toEqual([
      {
        action: "accept",
        content: {
          email: "ada@example.com",
          copies: 1,
          format: "html",
          sections: ["costs"],
        },
      },
      {
        action: "accept",
        content: {
          email: "ada@example.com",
          copies: 1,
          format: "html",
          sections: ["costs"],
          urgent: true,
        },
      },
      { action: "decline" },
      "cancelled",
    ]);
});

test("an MCP link is shown whole and opened only by the person", async ({
  page,
  session,
  request,
}) => {
  const commands = await servePending(page, request, session, LINK);
  let fetched = false;
  await page.route("https://auth.example.com/**", (route) => {
    fetched = true;
    return route.abort();
  });
  await page.goto(`/#session=${session.id}`);
  const card = page.getByRole("region", { name: "Pending decision" });
  await expect(card).toContainText("calendar asks: Connect your calendar.");
  await expect(card.locator(".elicit-url")).toHaveText(LINK.elicitation.url);
  await expect(card.locator(".elicit-url strong")).toHaveText(
    "auth.example.com",
  );
  await expect(card.getByText(/punycode/)).toHaveCount(0);
  const open = card.getByRole("link", {
    name: "Open auth.example.com in your browser",
  });
  await expect(open).toHaveAttribute("href", LINK.elicitation.url);
  await expect(open).toHaveAttribute("target", "_blank");
  await expect(open).toHaveAttribute("rel", "noopener noreferrer");
  expect(await uneven(page)).toEqual([]);
  await card.getByRole("button", { name: "Continue" }).click();
  await card.getByRole("button", { name: "Decline" }).click();
  await expect
    .poll(() => replies(commands))
    .toEqual([{ action: "accept" }, { action: "decline" }]);
  expect(fetched).toBe(false);
});

test("a punycode host is named as one", async ({ page, session, request }) => {
  await servePending(page, request, session, {
    ...LINK,
    elicitation: {
      ...LINK.elicitation,
      url: "https://xn--bnk-sna.example/",
      host: "xn--bnk-sna.example",
      punycode: true,
    },
  });
  await page.goto(`/#session=${session.id}`);
  await expect(
    page
      .getByRole("region", { name: "Pending decision" })
      .getByRole("status")
      .filter({ hasText: "internationalised (punycode) name" }),
  ).toBeVisible();
});
