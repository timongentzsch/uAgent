import { expect } from "@playwright/test";

// A control that stands in a corner of the box it is in is as far from one
// edge as from the other. Each that is not is named with both distances.

const CONTROLS = "button, select, a.chip, a.card, [role=button]";
// Closer than this to an edge, a control stands at it.
const NEAR = 12;

export const uneven = (page) =>
  page.evaluate(
    ([selector, near]) => {
      const shown = (node) => {
        const box = node.getBoundingClientRect();
        return (
          box.width > 0 &&
          box.height > 0 &&
          getComputedStyle(node).visibility !== "hidden" &&
          !node.closest("[inert], .xterm")
        );
      };
      const named = (node) =>
        `${node.tagName.toLowerCase()}${[...node.classList].map((name) => `.${name}`).join("")}`;
      // A colour with alpha left: not "transparent", nor a last "/ a" or
      // fourth "rgba(…, a)" argument of 0.
      const painted = (color) => {
        if (color === "transparent") return false;
        const parts = color.slice(color.indexOf("(") + 1, -1).split(/[\s,/]+/);
        const alpha = color.includes("/")
          ? parts.at(-1)
          : color.startsWith("rgba")
            ? parts[3]
            : "1";
        return Number.parseFloat(alpha) !== 0;
      };
      // The ground an element stands on: its own, or the nearest one under it.
      const ground = (node) => {
        for (let at = node; at; at = at.parentElement) {
          const color = getComputedStyle(at).backgroundColor;
          if (painted(color)) return color;
        }
      };
      // How far the control is from the nearest edge shown on that side: a
      // border there, or the end of a ground of its own.
      const gap = (node, side) => {
        const at = node.getBoundingClientRect();
        for (let box = node.parentElement; box; box = box.parentElement) {
          const style = getComputedStyle(box);
          const width = Number.parseFloat(style[`border${side}Width`]);
          const edged = width > 0 && style[`border${side}Style`] !== "none";
          const own =
            painted(style.backgroundColor) &&
            style.backgroundColor !== ground(box.parentElement);
          if (!edged && !own && box.parentElement) continue;
          const inner = box.getBoundingClientRect();
          const lower = side === "Left" || side === "Top";
          const key = side.toLowerCase();
          return {
            box,
            far: lower
              ? at[key] - inner[key] - width
              : inner[key] - width - at[key],
          };
        }
      };
      const found = [];
      for (const node of document.querySelectorAll(selector)) {
        if (!shown(node)) continue;
        const nearest = (a, b) => {
          const [one, two] = [gap(node, a), gap(node, b)];
          return one.far <= two.far ? one : two;
        };
        const across = nearest("Left", "Right");
        const down = nearest("Top", "Bottom");
        if (
          across.far > near ||
          down.far > near ||
          across.far < -1 ||
          down.far < -1 ||
          Math.abs(across.far - down.far) <= 1
        )
          continue;
        const label = (
          node.getAttribute("aria-label") ||
          node.textContent ||
          ""
        )
          .trim()
          .replace(/\s+/g, " ")
          .slice(0, 24);
        found.push(
          `${named(node)} "${label}": ${across.far.toFixed(1)} from the side of ${named(across.box)}, ${down.far.toFixed(1)} from the end of ${named(down.box)}`,
        );
      }
      return [...new Set(found)];
    },
    [CONTROLS, NEAR],
  );

// Runs a probe's checks in the light and the dark theme on the page as it
// stands, then gives the page back its scheme: a box may show in one only.
export const eachTheme = async (page, check) => {
  const html = page.locator("html");
  const was = await html.getAttribute("data-theme");
  for (const theme of ["light", "dark"]) {
    await page.emulateMedia({ colorScheme: theme });
    await expect(html).toHaveAttribute("data-theme", theme);
    await check();
  }
  await page.emulateMedia({ colorScheme: was });
  await expect(html).toHaveAttribute("data-theme", was);
};
