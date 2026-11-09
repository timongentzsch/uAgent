// Runs first in every page the Files pane previews (the host inlines it). The
// page is sandboxed in an opaque origin; this tells the app what the page is,
// what went wrong in it and what the person picked, by postMessage, and
// applies the tweaks the app sends. Opened alone with #paper it takes its
// paper size for printing; with #print it also prints.
// Once loaded, it reports what is wrong with the page as drawn (larger than
// its artboard, text over text on a slide, a page of a document that runs
// past it, a file that did not load): as errors to the app, or on the
// console of the agent's browser.
(() => {
  const root = document.documentElement;
  const meta = (name) =>
    document.querySelector(`meta[name="${name}"]`)?.content.trim() || "";
  // Paper in millimetres: a name, "landscape" after it, or WxHmm.
  const PAPER = {
    a3: [297, 420],
    a4: [210, 297],
    a5: [148, 210],
    letter: [215.9, 279.4],
    legal: [215.9, 355.6],
  };
  const paper = () => {
    const [flow, name = "a4", turn] = meta("print").toLowerCase().split(/\s+/);
    if (flow !== "flow") return null;
    const custom = /^(\d+(?:\.\d+)?)x(\d+(?:\.\d+)?)mm$/.exec(name);
    const [w, h] = custom ? [+custom[1], +custom[2]] : PAPER[name] || PAPER.a4;
    return turn === "landscape" ? { w: h, h: w } : { w, h };
  };
  const PX_PER_MM = 96 / 25.4;
  // The artboard ("1440x900", "1920x1080 slide", or a width only); a document
  // without one is its paper's width.
  const artboard = () => {
    const [, w, h, slide] =
      /^(\d+)(?:x(\d+))?(\s+slide)?$/.exec(meta("artboard")) || [];
    const sheet = paper();
    return {
      w: +w || (sheet ? Math.round(sheet.w * PX_PER_MM) : 0),
      h: +h || 0,
      slide: !!slide,
    };
  };
  const kind = () => (artboard().slide ? "slide" : paper() ? "doc" : "design");
  const tweaks = () => {
    try {
      return JSON.parse(
        document.getElementById("tweaks")?.textContent || "null",
      );
    } catch {
      return null;
    }
  };
  // A CSS variable, or a data- attribute for the page's CSS to select on.
  const tweak = (name, value) => {
    if (name.startsWith("--")) root.style.setProperty(name, String(value));
    else if (/^[a-z][a-z0-9-]{0,31}$/.test(name))
      root.dataset[name] = String(value);
  };
  // A slide's speaker notes: its last <aside>, never drawn.
  const notes = () =>
    artboard().slide
      ? [...document.querySelectorAll("body > aside")].pop() || null
      : null;
  addEventListener("DOMContentLoaded", () => {
    for (const [name, spec] of Object.entries(tweaks() || {}))
      if (!name.startsWith("--") && spec && "default" in spec)
        tweak(name, spec.default);
    notes()?.style.setProperty("display", "none", "important");
  });

  // A file the page links that did not load.
  const missing = [];
  addEventListener(
    "error",
    (event) => {
      const url = event.target?.src || event.target?.href;
      if (event.target !== window && url) missing.push(url);
    },
    true,
  );
  // The boxes of a text node's lines.
  const lines = (node) => {
    const range = document.createRange();
    range.selectNodeContents(node);
    return [...range.getClientRects()].filter((r) => r.width && r.height);
  };
  const crosses = (a, b) =>
    Math.min(a.right, b.right) - Math.max(a.left, b.left) > 1 &&
    Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > 1;
  // Text drawn over other text, or past the slide's edge (cut off there
  // when the page hides its overflow): on a slide neither may happen.
  const overlaps = () => {
    const { w, h } = artboard();
    const texts = [];
    const walk = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    while (walk.nextNode()) {
      const node = walk.currentNode;
      if (!node.textContent.trim() || node.parentElement.closest("aside"))
        continue;
      texts.push({ node, boxes: lines(node) });
    }
    const found = [];
    const words = (node) => `“${node.textContent.trim().slice(0, 40)}”`;
    for (const { node, boxes } of texts)
      if (
        found.length < 5 &&
        boxes.some((box) => box.bottom > h + 1 || box.right > w + 1)
      )
        found.push(
          `${words(node)} runs past the slide's ${w}x${h} edge: shorten it, or split the slide`,
        );
    for (let i = 0; i < texts.length && found.length < 5; i++)
      for (let j = i + 1; j < texts.length; j++)
        if (
          texts[i].boxes.some((a) => texts[j].boxes.some((b) => crosses(a, b)))
        ) {
          found.push(
            `${words(texts[i].node)} overlaps ${words(texts[j].node)}: give them room, or split the slide`,
          );
          break;
        }
    return found;
  };
  // A length the page's CSS gives, in pixels.
  const pixels = (length) => {
    const probe = document.createElement("div");
    probe.style.cssText = `position:absolute;visibility:hidden;height:${length}`;
    document.body.append(probe);
    const value = probe.offsetHeight;
    probe.remove();
    return value;
  };
  // The height of a page's text: its paper less the @page margins the
  // page's own <style> gives.
  const pageHeight = () => {
    let margins = 0;
    for (const style of document.styleSheets) {
      try {
        const rule = [...style.cssRules].find((r) => r instanceof CSSPageRule);
        if (rule)
          margins =
            pixels(rule.style.marginTop || "0") +
            pixels(rule.style.marginBottom || "0");
      } catch {
        // A linked sheet from another origin: its rules are not readable.
      }
    }
    return paper().h * PX_PER_MM - margins;
  };
  // A block that cannot break and is taller than a page of the document.
  const tallBlocks = () => {
    const page = pageHeight();
    return [...document.querySelectorAll("figure, table, img, svg, pre")]
      .filter((block) => !block.parentElement.closest("figure, table, svg"))
      .filter((block) => block.getBoundingClientRect().height > page + 1)
      .map(
        (block) =>
          `a ${block.localName} is ${Math.round(block.getBoundingClientRect().height)}px tall, more than a page (${Math.round(page)}px): shrink it or split it`,
      );
  };
  const problems = () => {
    const found = missing.map((url) => `${url} did not load`);
    const { w, h, slide } = artboard();
    if (w && h && (root.scrollWidth > w + 1 || root.scrollHeight > h + 1))
      found.push(
        `the page is ${root.scrollWidth}x${root.scrollHeight}, larger than its ${w}x${h} artboard: make it fit, or for a page that scrolls give the artboard only its width (content="${w}")`,
      );
    document.querySelectorAll("body > section").forEach((page, at) => {
      if (page.scrollHeight > page.clientHeight + 1)
        found.push(
          `page ${at + 1} runs ${page.scrollHeight - page.clientHeight}px past its height`,
        );
    });
    if (slide) found.push(...overlaps());
    if (paper()) found.push(...tallBlocks());
    return found;
  };
  const checked = (report) =>
    addEventListener("load", () =>
      document.fonts.ready.then(() => problems().forEach(report)),
    );

  // Its paper for printing: a fixed artboard is one sheet its size, a
  // document its paper, with its own @page margins kept.
  const paperStyle = () => {
    const { w, h } = artboard();
    const sheet = paper();
    const style = document.createElement("style");
    style.textContent =
      w && h
        ? `@page{size:${w}px ${h}px;margin:0}`
        : sheet
          ? `@page{size:${sheet.w}mm ${sheet.h}mm}`
          : "";
    document.head.prepend(style);
  };
  // Printed as drawn, backgrounds too, also in a deck's frames, which a
  // printer's own background setting does not reach.
  addEventListener("DOMContentLoaded", () => {
    const colors = document.createElement("style");
    colors.textContent =
      "html{print-color-adjust:exact;-webkit-print-color-adjust:exact}";
    document.head.prepend(colors);
  });

  if (parent === window) {
    checked((problem) => console.warn(`uagent page: ${problem}`));
    if (location.hash !== "#print" && location.hash !== "#paper") return;
    addEventListener("DOMContentLoaded", paperStyle);
    if (location.hash === "#print")
      addEventListener("load", async () => {
        await document.fonts.ready;
        print();
      });
    return;
  }

  // A document's text blocks, in order: what a person edits in place.
  const EDITABLE =
    "h1, h2, h3, h4, h5, h6, p, li, td, th, dt, dd, figcaption, blockquote";
  const blocks = () =>
    [...document.body.querySelectorAll(EDITABLE)].filter(
      (block) => !block.parentElement.closest(EDITABLE),
    );
  // Where print will start each page after the first, estimated from the
  // blocks' heights: one that cannot break moves whole to the next page,
  // and a heading goes with what follows it. The PDF is the exact one.
  const ATOMIC = "figure, img, svg, pre, tr, h1, h2, h3, h4, h5, h6, li";
  const pageBreaks = () => {
    const height = pageHeight();
    const top = (el) => el.getBoundingClientRect().top + scrollY;
    const bottom = (el) => el.getBoundingClientRect().bottom + scrollY;
    const parts = [
      ...document.body.querySelectorAll(`${ATOMIC}, p, blockquote, dt, dd`),
    ].filter((el) => !el.parentElement.closest(ATOMIC) && el.offsetHeight);
    const breaks = [];
    let start = parts.length ? top(parts[0]) : 0;
    let previous = null;
    for (const part of parts) {
      const forced = getComputedStyle(part).breakBefore === "page";
      if (!forced && bottom(part) <= start + height) {
        previous = part;
        continue;
      }
      let at = forced || part.matches(ATOMIC) ? top(part) : start + height;
      if (/^H[1-6]$/.test(previous?.tagName || "") && at === top(part))
        at = top(previous);
      if (at <= start) at = start + height;
      breaks.push(Math.round(at));
      start = at;
      while (bottom(part) > start + height) {
        start += height;
        breaks.push(Math.round(start));
      }
      previous = part;
    }
    return breaks;
  };
  // The page as Markdown: its text, headings, lists, tables, links and images.
  const markdown = () => {
    const inline = (node) =>
      [...node.childNodes]
        .map((child) => {
          if (child.nodeType === Node.TEXT_NODE)
            return child.textContent.replace(/\s+/g, " ");
          if (child.nodeType !== Node.ELEMENT_NODE) return "";
          const text = inline(child);
          switch (child.localName) {
            case "b":
            case "strong":
              return text.trim() ? `**${text.trim()}**` : "";
            case "i":
            case "em":
              return text.trim() ? `*${text.trim()}*` : "";
            case "code":
              return `\`${child.textContent}\``;
            case "a":
              return `[${text.trim()}](${child.getAttribute("href") || ""})`;
            case "img":
              return `![${child.alt || ""}](${child.getAttribute("src") || ""})`;
            case "br":
              return "  \n";
            case "svg":
            case "script":
            case "style":
              return "";
            default:
              return text;
          }
        })
        .join("");
    const block = (node, depth = 0) => {
      const out = [];
      for (const child of node.children) {
        const tag = child.localName;
        const text = () => inline(child).trim();
        if (/^h[1-6]$/.test(tag)) out.push(`${"#".repeat(+tag[1])} ${text()}`);
        else if (tag === "p" || tag === "figcaption") out.push(text());
        else if (tag === "blockquote") out.push(text().replace(/^/gm, "> "));
        else if (tag === "pre")
          out.push(`\`\`\`\n${child.textContent.replace(/\n$/, "")}\n\`\`\``);
        else if (tag === "hr") out.push("---");
        else if (tag === "img") out.push(inline({ childNodes: [child] }));
        else if (tag === "ul" || tag === "ol")
          out.push(
            [...child.children]
              .map(
                (item, at) =>
                  `${"  ".repeat(depth)}${tag === "ol" ? `${at + 1}.` : "-"} ${inline(item).trim()}` +
                  [...item.children]
                    .filter(
                      (sub) => sub.localName === "ul" || sub.localName === "ol",
                    )
                    .map((sub) => `\n${block({ children: [sub] }, depth + 1)}`)
                    .join(""),
              )
              .join("\n"),
          );
        else if (tag === "table") {
          const rows = [...child.querySelectorAll("tr")].map((row) =>
            [...row.children].map((cell) =>
              inline(cell).trim().replace(/\|/g, "\\|"),
            ),
          );
          if (rows.length)
            out.push(
              [
                `| ${rows[0].join(" | ")} |`,
                `| ${rows[0].map(() => "---").join(" | ")} |`,
                ...rows.slice(1).map((cells) => `| ${cells.join(" | ")} |`),
              ].join("\n"),
            );
        } else if (
          !["script", "style", "aside", "svg", "template"].includes(tag)
        ) {
          const inner = block(child, depth);
          if (inner) out.push(inner);
        }
      }
      return out.filter(Boolean).join("\n\n");
    };
    return `${block(document.body)}\n`;
  };
  // Edit: text blocks take typing; a block left changed is reported with its
  // place and its text before, which the app checks against the file.
  const before = new WeakMap();
  const focused = (event) => {
    const block = event.target.closest?.("[contenteditable]");
    if (block && !before.has(block))
      before.set(block, { html: block.innerHTML, text: block.textContent });
  };
  const left = (event) => {
    const block = event.target.closest?.("[contenteditable]");
    const was = block && before.get(block);
    if (!was || was.html === block.innerHTML) return;
    before.delete(block);
    send("edit", {
      index: blocks().indexOf(block),
      before: was.text,
      html: block.innerHTML,
    });
  };
  const edit = (on) => {
    for (const block of blocks())
      if (on) block.setAttribute("contenteditable", "plaintext-only");
      else block.removeAttribute("contenteditable");
    const toggle = on ? addEventListener : removeEventListener;
    toggle("focusin", focused, true);
    toggle("focusout", left, true);
  };

  const send = (type, data) =>
    parent.postMessage({ uagentDesign: 1, type, ...data }, "*");
  const height = () => root.scrollHeight;
  checked((problem) => send("error", { message: problem, line: 0 }));
  const failed = (message, line) =>
    send("error", {
      message: String(message || "error").slice(0, 300),
      line: line || 0,
    });
  addEventListener("error", (event) => failed(event.message, event.lineno));
  addEventListener("unhandledrejection", (event) =>
    failed(event.reason?.message || event.reason, 0),
  );
  addEventListener("DOMContentLoaded", () => {
    send("ready", {
      title: document.title,
      artboard: artboard(),
      kind: kind(),
      notes: notes()?.textContent.trim() || "",
      tweaks: tweaks(),
      height: height(),
      // Where its stylesheets are, so the app can read a design system's.
      sheets: [...document.querySelectorAll('link[rel="stylesheet"]')].map(
        (link) => link.href,
      ),
    });
    let last = 0;
    new ResizeObserver(() => {
      if (height() !== last) send("size", { height: (last = height()) });
      if (paper()) send("pages", { breaks: pageBreaks() });
    }).observe(root);
  });

  // Select: the pointer marks an element, a click picks it and does nothing
  // else. The app draws both, from the element's box.
  const box = (element) => {
    const { x, y, width, height } = element.getBoundingClientRect();
    return { x, y, width, height };
  };
  const step = (element) => {
    if (element.id) return `#${CSS.escape(element.id)}`;
    const tag = element.localName;
    const same = [...element.parentElement.children].filter(
      (e) => e.localName === tag,
    );
    return same.length > 1
      ? `${tag}:nth-of-type(${same.indexOf(element) + 1})`
      : tag;
  };
  const selector = (element) => {
    const steps = [];
    for (
      let at = element;
      at && at !== document.body && at !== root;
      at = at.parentElement
    ) {
      steps.unshift(step(at));
      if (at.id) break;
    }
    return steps.join(" > ") || "body";
  };
  let picked = null;
  let frame = 0;
  const hover = (event) => {
    cancelAnimationFrame(frame);
    frame = requestAnimationFrame(() =>
      send("hover", { rect: box(event.target) }),
    );
  };
  const pick = (event) => {
    event.preventDefault();
    event.stopPropagation();
    picked = event.target;
    send("pick", {
      rect: box(picked),
      selector: selector(picked),
      tag: picked.localName,
      text: (
        picked.innerText ||
        picked.getAttribute("alt") ||
        picked.getAttribute("aria-label") ||
        ""
      )
        .replace(/\s+/g, " ")
        .trim()
        .slice(0, 60),
    });
  };
  const moved = () => picked && send("picked", { rect: box(picked) });
  const select = (on) => {
    const toggle = on ? addEventListener : removeEventListener;
    toggle("pointermove", hover, true);
    toggle("click", pick, true);
    toggle("scroll", moved, true);
    root.style.cursor = on ? "crosshair" : "";
    if (!on) picked = null;
  };
  // A zoom over the page is the pane's: ⌘ or Ctrl and the wheel (a
  // trackpad pinch), or Safari's pinch, at the pointer.
  const zoom = (by, event) =>
    send("zoom", { by, x: event.clientX, y: event.clientY });
  addEventListener(
    "wheel",
    (event) => {
      if (!(event.ctrlKey || event.metaKey)) return;
      event.preventDefault();
      zoom(Math.exp(-event.deltaY * 0.01), event);
    },
    { passive: false },
  );
  let pinch = 1;
  for (const kind of ["gesturestart", "gesturechange"])
    addEventListener(kind, (event) => {
      event.preventDefault();
      if (event.type === "gesturechange") zoom(event.scale / pinch, event);
      pinch = event.type === "gesturestart" ? 1 : event.scale;
    });
  addEventListener("keydown", (event) => {
    const field = event.target.closest?.(
      "input, textarea, select, [contenteditable]",
    );
    if (!field && ["Escape", "ArrowLeft", "ArrowRight"].includes(event.key))
      send("key", { key: event.key });
  });
  addEventListener("message", (event) => {
    if (event.source !== parent || event.data?.uagentDesign !== 1) return;
    if (event.data.type === "select") select(!!event.data.on);
    if (event.data.type === "edit" && paper()) edit(!!event.data.on);
    if (event.data.type === "markdown") send("markdown", { text: markdown() });
    if (event.data.type === "tweak")
      tweak(String(event.data.name), event.data.value);
  });
})();
