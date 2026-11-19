// What a press shows: every control on the page is pressed in turn, as
// :active sets it, and those that look the same pressed are named. Two are
// left out: the skip link, which only a keyboard reaches, and the current
// picture, which wears the ring already.

const CONTROLS =
  "button, a[href], summary, label:has(input:is([type=checkbox], [type=radio], [type=file])), [role=button], [role=tab], [role=option], [role^=menuitem], [role=treeitem]";
const EXEMPT = '.skip-link, .cover[aria-selected="true"]';

// Tags each control shown and enabled, and returns their names.
const tag = (page) =>
  page.evaluate(
    ([selector, exempt]) => {
      const shown = (node) => {
        const box = node.getBoundingClientRect();
        return (
          box.width > 0 &&
          box.height > 0 &&
          getComputedStyle(node).visibility !== "hidden" &&
          !node.closest("[inert], .xterm")
        );
      };
      const top = [...document.querySelectorAll("dialog[open]")]
        .filter((dialog) => dialog.matches(":modal"))
        .at(-1);
      const found = [];
      for (const node of (top || document).querySelectorAll(selector)) {
        if (
          !shown(node) ||
          node.matches(`:disabled, [aria-disabled=true], ${exempt}`)
        )
          continue;
        node.dataset.press = found.length;
        found.push(
          `${node.tagName.toLowerCase()}.${[...node.classList].join(".")} "${(
            node.getAttribute("aria-label") ||
            node.textContent ||
            ""
          )
            .trim()
            .replace(/\s+/g, " ")
            .slice(0, 40)}"`,
        );
      }
      return found;
    },
    [CONTROLS, EXEMPT],
  );

// What is painted on and around the tagged control: itself, what it holds,
// what stands beside it and the rows that hold it.
const painted = (page, index) =>
  page.evaluate((index) => {
    const node = document.querySelector(`[data-press="${index}"]`);
    const look = (element, pseudo) => {
      const style = getComputedStyle(element, pseudo);
      return [
        "background-color",
        "background-image",
        "color",
        "border-top-color",
        "box-shadow",
        "opacity",
        "filter",
        "scale",
        "outline-color",
        "outline-style",
      ]
        .map((property) => style.getPropertyValue(property))
        .join("|");
    };
    const around = [
      node,
      ...node.querySelectorAll("*"),
      ...node.parentElement.children,
    ].slice(0, 30);
    for (let at = node.parentElement, up = 0; at && up < 3; up++) {
      around.push(at);
      at = at.parentElement;
    }
    return around
      .flatMap((element) => [
        look(element),
        look(element, "::before"),
        look(element, "::after"),
      ])
      .join("\n");
  }, index);

// The controls on the page that show nothing when pressed.
export async function unanswered(page) {
  const names = await tag(page);
  const client = await page.context().newCDPSession(page);
  await client.send("DOM.enable");
  await client.send("CSS.enable");
  const { root } = await client.send("DOM.getDocument", { depth: 0 });
  const silent = [];
  for (let index = 0; index < names.length; index++) {
    const { nodeId } = await client.send("DOM.querySelector", {
      nodeId: root.nodeId,
      selector: `[data-press="${index}"]`,
    });
    if (!nodeId) continue;
    const rest = await painted(page, index);
    await client.send("CSS.forcePseudoState", {
      nodeId,
      forcedPseudoClasses: ["active"],
    });
    const pressed = await painted(page, index);
    await client.send("CSS.forcePseudoState", {
      nodeId,
      forcedPseudoClasses: [],
    });
    if (rest === pressed) silent.push(names[index]);
  }
  await page.evaluate(() => {
    for (const node of document.querySelectorAll("[data-press]"))
      delete node.dataset.press;
  });
  await client.detach();
  return [...new Set(silent)];
}
