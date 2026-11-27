import { expect } from "@playwright/test";
import { uneven } from "./insets.js";
import { unanswered } from "./press.js";

// What is cramped on the page now, each named so a failure says where.
const cramped = (page) =>
  page.evaluate(() => {
    const found = [];
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
      (node.getAttribute("aria-label") || node.textContent || node.className)
        .trim()
        .replace(/\s+/g, " ")
        .slice(0, 40);
    // A box the user scrolls on purpose, sideways.
    const scrolls = (node) =>
      /auto|scroll/.test(getComputedStyle(node).overflowX);
    const scroller = (node) => {
      for (let at = node.parentElement; at; at = at.parentElement)
        if (scrolls(at) && at !== document.documentElement) return at;
      return null;
    };
    if (document.documentElement.scrollWidth > innerWidth + 1)
      found.push(
        `the page scrolls sideways by ${document.documentElement.scrollWidth - innerWidth}px`,
      );
    for (const button of document.querySelectorAll("button")) {
      if (!shown(button)) continue;
      const box = button.getBoundingClientRect();
      if (!scroller(button) && (box.left < -1 || box.right > innerWidth + 1))
        found.push(`off the screen: ${named(button)}`);
      // A label that wrapped: one text on two lines. A row may have a
      // line of its own for its details.
      const walk = document.createTreeWalker(button, NodeFilter.SHOW_TEXT);
      for (let text = walk.nextNode(); text; text = walk.nextNode()) {
        if (!text.textContent.trim()) continue;
        const range = document.createRange();
        range.selectNodeContents(text);
        const lines = new Set(
          [...range.getClientRects()]
            .filter((rect) => rect.width)
            .map((rect) => Math.round(rect.top / 4)),
        );
        if (lines.size > 1) {
          found.push(`wraps: ${named(button)}`);
          break;
        }
      }
      // Cut without the ellipsis that says so: a name that may be long is
      // cut on purpose.
      for (const part of [button, ...button.querySelectorAll("*")])
        if (
          shown(part) &&
          part.clientWidth > 1 &&
          part.scrollWidth > part.clientWidth + 1 &&
          getComputedStyle(part).textOverflow !== "ellipsis"
        ) {
          found.push(`cut: ${named(button)}`);
          break;
        }
    }
    // A row of buttons that its box does not hold.
    const rows = new Set();
    for (const button of document.querySelectorAll("button"))
      if (shown(button) && button.parentElement) rows.add(button.parentElement);
    for (const row of rows) {
      if (scrolls(row) || scroller(row)) continue;
      const buttons = [...row.children].filter(
        (child) => child.tagName === "BUTTON" && shown(child),
      );
      if (buttons.length < 2) continue;
      const box = row.getBoundingClientRect();
      if (
        buttons.some(
          (button) => button.getBoundingClientRect().right > box.right + 1,
        )
      )
        found.push(`a row wider than its box: ${named(row)}`);
    }
    // A row's press covers its line: nothing to read stands beside the
    // button on it but another control.
    for (const row of document.querySelectorAll("button, summary, a[href]")) {
      if (!shown(row) || getComputedStyle(row).justifyContent !== "flex-start")
        continue;
      const box = row.getBoundingClientRect();
      for (const beside of row.parentElement.children) {
        const at = beside.getBoundingClientRect();
        if (
          beside !== row &&
          shown(beside) &&
          !beside.matches("button, a, input, select, summary, label") &&
          !beside.querySelector("button, a, input, select, summary") &&
          at.top < box.bottom &&
          at.bottom > box.top &&
          beside.textContent.trim()
        )
          found.push(`a press stops short of: ${named(beside)}`);
      }
    }
    // Whether a scroller holding the box can still move it off that side:
    // a divider scrolling past the edge is no frame line.
    const passing = (box, side) => {
      for (let at = box.parentElement; at; at = at.parentElement) {
        const scroll = {
          Top: at.scrollTop,
          Left: at.scrollLeft,
          Right: at.scrollWidth - at.clientWidth - at.scrollLeft,
          Bottom: at.scrollHeight - at.clientHeight - at.scrollTop,
        }[side];
        if (scroll > 1 && !/sticky|fixed/.test(getComputedStyle(box).position))
          return true;
      }
      return false;
    };
    // A line along the screen's edge, which a phone's round corners cut.
    for (const box of document.querySelectorAll("#app *, dialog[open]")) {
      if (!shown(box) || !box.checkVisibility({ opacityProperty: true }))
        continue;
      const style = getComputedStyle(box);
      const at = box.getBoundingClientRect();
      for (const [side, edge] of [
        ["Top", at.top],
        ["Left", at.left],
        ["Right", innerWidth - at.right],
        ["Bottom", innerHeight - at.bottom],
      ])
        if (
          Math.abs(edge) < 1 &&
          !passing(box, side) &&
          Number.parseFloat(style[`border${side}Width`]) > 0 &&
          style[`border${side}Style`] !== "none" &&
          !/, 0\)$|^transparent$/.test(style[`border${side}Color`])
        )
          found.push(
            `a line along the screen's ${side.toLowerCase()} edge: ${named(box)}`,
          );
    }
    for (const dialog of document.querySelectorAll(
      "dialog[open], .dialog-body",
    ))
      if (shown(dialog) && dialog.scrollWidth > dialog.clientWidth + 1)
        found.push(`a dialog scrolls sideways: ${named(dialog)}`);
    return [...new Set(found)];
  });

export const roomy = async (page) => {
  await expect.poll(() => cramped(page)).toEqual([]);
  expect(await uneven(page)).toEqual([]);
  // The press probe reads styles through CDP, which only Chromium has.
  if (page.context().browser().browserType().name() === "chromium")
    expect(await unanswered(page)).toEqual([]);
};
