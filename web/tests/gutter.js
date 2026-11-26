// A settings page on a phone: what breaks its one left and one right edge,
// its touch sizes or its bottom inset, each named so a failure says where.
// The edge is where the first caption's text starts; the right edge mirrors
// it.

export const offGutter = (page, root = ".settings-pane") =>
  page.evaluate((selector) => {
    const pane = document.querySelector(selector);
    const found = [];
    if (!pane) return ["no settings page"];
    const shown = (node) => {
      const box = node.getBoundingClientRect();
      return (
        box.width > 0 &&
        box.height > 0 &&
        getComputedStyle(node).visibility !== "hidden" &&
        !node.closest("[inert], .placeholder")
      );
    };
    const named = (node) =>
      `${node.tagName.toLowerCase()}${[...node.classList].map((name) => `.${name}`).join("")} "${(node.getAttribute("aria-label") || node.textContent || "").trim().replace(/\s+/g, " ").slice(0, 24)}"`;
    const px = (name) => {
      const probe = document.createElement("div");
      probe.style.width = `var(${name})`;
      pane.append(probe);
      const width = probe.getBoundingClientRect().width;
      probe.remove();
      return width;
    };
    // Where text starts on a line of `node`, and where it ends.
    const textEdges = (node) => {
      const range = document.createRange();
      range.selectNodeContents(node);
      const rects = [...range.getClientRects()].filter((rect) => rect.width);
      if (!rects.length) return null;
      return {
        left: Math.min(...rects.map((rect) => rect.left)),
        right: Math.max(...rects.map((rect) => rect.right)),
      };
    };
    const caption = pane.querySelector(".section-title");
    const start = caption && textEdges(caption)?.left;
    if (start === undefined || start === null) return ["no caption"];
    const paneBox = pane.getBoundingClientRect();
    const end =
      paneBox.right -
      (start - paneBox.left) -
      (pane.offsetWidth - pane.clientWidth);
    const near = (a, b) => Math.abs(a - b) <= 1;
    // Nothing nearer the screen's edges than the gutter.
    if (start < px("--inset-x") * 2 - 1)
      found.push(`the gutter is ${start.toFixed(1)}px`);
    // Where things start: each on the one left edge.
    const starts = [
      ...pane.querySelectorAll(
        ".section-title, .row-label, .row-detail, .group-footer, .settings-scope, .text-control, .row-value > .sheet-control > button, .row-value > .segmented, .rule-form > *",
      ),
    ].filter(
      (node) =>
        shown(node) &&
        !node.closest(
          ".row-icon ~ *, .mcp-detail, .context, .tool-groups, .tools-content, dl.stats, .provider-form",
        ) &&
        !node.closest(".row")?.querySelector(":scope > .row-icon"),
    );
    for (const node of starts) {
      const text = node.matches(
        ".row-label, .row-detail, .section-title, .group-footer",
      );
      const left = text
        ? textEdges(node)?.left
        : node.getBoundingClientRect().left;
      if (left != null && !near(left, start))
        found.push(
          `starts at ${left.toFixed(1)}, not ${start.toFixed(1)}: ${named(node)}`,
        );
    }
    // Fields and the lines between rows end on the one right edge.
    for (const node of pane.querySelectorAll(
      ".text-control, .row-value > .sheet-control > button, .settings-scope",
    ))
      if (
        shown(node) &&
        !node.closest(
          ".provider-form, .rule-form .input-row, .tools-content",
        ) &&
        !near(node.getBoundingClientRect().right, end)
      )
        found.push(
          `ends at ${node.getBoundingClientRect().right.toFixed(1)}, not ${end.toFixed(1)}: ${named(node)}`,
        );
    for (const row of pane.querySelectorAll(
      ".group-rows > :not(:first-child)",
    )) {
      if (!shown(row)) continue;
      const style = getComputedStyle(row);
      if (
        style.borderTopStyle !== "none" &&
        Number.parseFloat(style.borderTopWidth) > 0
      )
        found.push(`a line from edge to edge: ${named(row)}`);
      if (style.backgroundImage === "none") continue;
      const box = row.getBoundingClientRect();
      const inset = Number.parseFloat(style.backgroundPositionX);
      if (!near(box.left + inset, start))
        found.push(
          `a line starts at ${(box.left + inset).toFixed(1)}: ${named(row)}`,
        );
    }
    // A choice under its text reads as a field: filled, its text at the
    // start.
    for (const select of pane.querySelectorAll(".row-value select")) {
      if (!shown(select)) continue;
      const style = getComputedStyle(select);
      if (
        !/start|left/.test(style.textAlign) ||
        /, 0\)$|^transparent$/.test(style.backgroundColor)
      )
        found.push(`a choice that is no field: ${named(select)}`);
    }
    // Every control at least the app's smallest touch size; fields at its
    // control size.
    const slim = px("--control-slim");
    const control = px("--control");
    for (const node of pane.querySelectorAll("button, input, select")) {
      if (!shown(node) || node.matches("input[type=checkbox]")) continue;
      const height = node.getBoundingClientRect().height;
      const field = node.matches(
        "input, select, .row-value > .sheet-control > button, .segmented > button",
      );
      if (height < (field ? control : slim) - 1)
        found.push(`${height.toFixed(1)}px tall: ${named(node)}`);
    }
    // Text that runs past its box without saying so.
    for (const node of pane.querySelectorAll("*")) {
      if (!shown(node) || node.matches(".text-control, .text-control *"))
        continue;
      const style = getComputedStyle(node);
      if (
        /hidden|clip/.test(style.overflowX) &&
        node.scrollWidth > node.clientWidth + 1 &&
        style.textOverflow !== "ellipsis"
      )
        found.push(`cut: ${named(node)}`);
    }
    if (pane.scrollWidth > pane.clientWidth + 1)
      found.push(
        `the page scrolls sideways by ${pane.scrollWidth - pane.clientWidth}px`,
      );
    // Scrolled to its end, the last row clears the home indicator.
    pane.scrollTop = pane.scrollHeight;
    const last = [...pane.querySelectorAll(".row, .group-footer, .group-block")]
      .filter(shown)
      .at(-1);
    const bottom = innerHeight - px("--safe-bottom");
    if (last && last.getBoundingClientRect().bottom > bottom + 1)
      found.push(
        `the last row ends at ${last.getBoundingClientRect().bottom.toFixed(1)}, under ${bottom.toFixed(1)}`,
      );
    pane.scrollTop = 0;
    return [...new Set(found)];
  }, root);
