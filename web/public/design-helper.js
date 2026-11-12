// Runs first in every page the Files pane previews (the host inlines it): by
// postMessage it says what the page is and what fails in it, and takes tweaks.
(() => {
  const root = document.documentElement;
  const meta = (name) =>
    document.querySelector(`meta[name="${name}"]`)?.content.trim() || "";
  // A document's print line: "flow", its paper (a name or WxHmm), "landscape",
  // then its margins: a preset, or one to four lengths as CSS reads them.
  const PAPER = {
    a3: [297, 420],
    a4: [210, 297],
    a5: [148, 210],
    letter: [215.9, 279.4],
    legal: [215.9, 355.6],
  };
  const MARGINS = {
    normal: [25],
    narrow: [12.7],
    moderate: [25, 19],
    wide: [25, 50],
  };
  // In millimetres, the margins from the top clockwise.
  const paper = () => {
    const [flow, name = "a4", ...more] = meta("print")
      .toLowerCase()
      .split(/\s+/);
    if (flow !== "flow") return null;
    const custom = /^(\d+(?:\.\d+)?)x(\d+(?:\.\d+)?)mm$/.exec(name);
    const [w, h] = custom ? [+custom[1], +custom[2]] : PAPER[name] || PAPER.a4;
    const turned = more[0] === "landscape";
    const said = more.slice(turned ? 1 : 0);
    const lengths = said.map(Number.parseFloat);
    const [top, right = top, bottom = top, left = right] =
      MARGINS[said[0]] ??
      (lengths.length && lengths.every(Number.isFinite)
        ? lengths
        : MARGINS.normal);
    return {
      w: turned ? h : w,
      h: turned ? w : h,
      margin: [top, right, bottom, left],
    };
  };
  const PX_PER_MM = 96 / 25.4;
  // The artboard ("1440x900", "1920x1080 slide", or a width only); a
  // document's is one sheet of its paper.
  const artboard = () => {
    const [, w, h, slide] =
      /^(\d+)(?:x(\d+))?(\s+slide)?$/.exec(meta("artboard")) || [];
    const sheet = paper();
    return {
      w: +w || (sheet ? Math.round(sheet.w * PX_PER_MM) : 0),
      h: +h || (sheet ? Math.round(sheet.h * PX_PER_MM) : 0),
      slide: !!slide,
    };
  };
  // Stepped through with its neighbours, and with nothing to scroll.
  const stepped = () => artboard().slide || !!paper();
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
  // The height of a page's text: its paper less the margins.
  const pageHeight = () => {
    const { h, margin } = paper();
    return (h - margin[0] - margin[2]) * PX_PER_MM;
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
    if (
      !paper() &&
      w &&
      h &&
      (root.scrollWidth > w + 1 || root.scrollHeight > h + 1)
    )
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
    addEventListener("load", async () => {
      await document.fonts.ready;
      // A sheet's cells in error, once it is calculated.
      const cells = (await window.uagentReady) ?? [];
      [...problems(), ...cells].forEach(report);
    });

  // Where each page of a document starts, from its body's top, estimated
  // from its blocks: one that cannot break moves whole to the next page, a
  // heading goes with what follows it, text breaks between two of its lines.
  const ATOMIC =
    "figure, img, svg, pre, tr, h1, h2, h3, h4, h5, h6, li, .title-page";
  const pageStarts = () => {
    const height = pageHeight();
    const from = document.body.getBoundingClientRect().top;
    const top = (box) => box.getBoundingClientRect().top - from;
    const bottom = (box) => box.getBoundingClientRect().bottom - from;
    // The top of the first line that runs past `limit`; the part's own when
    // none of its lines fits.
    const line = (part, limit) => {
      const range = document.createRange();
      range.selectNodeContents(part);
      const boxes = [...range.getClientRects()];
      const cut = boxes.findIndex((box) => box.bottom - from > limit);
      return cut > 0 ? boxes[cut].top - from : cut ? limit : top(part);
    };
    const parts = [
      ...document.body.querySelectorAll(`${ATOMIC}, p, blockquote, dt, dd`),
    ].filter((el) => !el.parentElement.closest(ATOMIC) && el.offsetHeight);
    const starts = [parts.length ? top(parts[0]) : 0];
    let previous = null;
    for (const part of parts) {
      const after = previous && getComputedStyle(previous).breakAfter;
      if (
        top(part) > starts.at(-1) &&
        (getComputedStyle(part).breakBefore === "page" || after === "page")
      )
        starts.push(top(part));
      while (bottom(part) > starts.at(-1) + height + 0.5) {
        const limit = starts.at(-1) + height;
        let at = part.matches(ATOMIC) ? top(part) : line(part, limit);
        if (/^H[1-6]$/.test(previous?.tagName || "") && at === top(part))
          at = top(previous);
        starts.push(at > starts.at(-1) ? at : limit);
      }
      previous = part;
    }
    return starts;
  };
  // A document's paper. In print its size and margins are the format's; on
  // screen it is one sheet, the body its text box inside the margins, moved
  // and cut to the page the address names ("#3").
  let tops = [0];
  let shown = 0;
  let paged;
  const turn = () => {
    const { margin } = paper();
    tops = pageStarts();
    const said = Number(location.hash.slice(1)) || 1;
    shown = Math.min(tops.length, Math.max(1, said)) - 1;
    const end = tops[shown + 1];
    root.style.setProperty(
      "--uagent-top",
      `${margin[0] * PX_PER_MM - tops[shown]}px`,
    );
    root.style.setProperty(
      "--uagent-cut",
      `inset(${tops[shown]}px -100vw ${end ? `calc(100% - ${end}px)` : "0px"} -100vw)`,
    );
    paged?.(tops.length);
  };
  addEventListener("DOMContentLoaded", () => {
    const format = paper();
    if (!format) return;
    const { w, h, margin } = format;
    const [top, right, bottom, left] = margin;
    const style = document.createElement("style");
    style.textContent =
      `@page{size:${w}mm ${h}mm;margin:${margin.map((side) => `${side}mm`).join(" ")}}` +
      `.title-page{box-sizing:border-box;height:${h - top - bottom}mm;break-after:page}` +
      `@media screen{html{width:${w}mm;height:${h}mm;overflow:hidden}` +
      `body{position:absolute;top:var(--uagent-top);left:${left}mm;width:${w - left - right}mm;` +
      "margin:0!important;padding:0!important;clip-path:var(--uagent-cut)}}";
    // After the page's own, which it overrules.
    document.head.append(style);
    new ResizeObserver(turn).observe(document.body);
    addEventListener("hashchange", turn);
  });
  // For printing, a fixed artboard is one sheet its size.
  const paperStyle = () => {
    const { w, h } = artboard();
    if (paper() || !w || !h) return;
    const style = document.createElement("style");
    style.textContent = `@page{size:${w}px ${h}px;margin:0}`;
    document.head.prepend(style);
  };
  // Printed as drawn, backgrounds too, also in a deck's frames, which a
  // printer's own background setting does not reach. A sheet's tables are
  // not shown as written while it is busy being drawn.
  addEventListener("DOMContentLoaded", () => {
    const colors = document.createElement("style");
    colors.textContent =
      "html{print-color-adjust:exact;-webkit-print-color-adjust:exact}" +
      "[data-uagent-busy] table[data-sheet]{visibility:hidden}";
    document.head.prepend(colors);
  });

  // A sheet is drawn and calculated by the code installed with its engine,
  // loaded from the page's own address. Until that is done the page says it
  // is busy, which a capture and the agent's browser wait out.
  addEventListener("DOMContentLoaded", () => {
    if (!meta("sheet")) return;
    const base = location.pathname.replace(/^(\/preview\/[^/]+\/).*$/, "$1");
    root.dataset.uagentBusy = "";
    window.uagentReady = import(`${base}~sheet/sheet.js`)
      .then((sheet) => sheet.ready)
      .catch((error) => {
        // Without the engine the tables still read, formulas as text.
        const note = document.createElement("p");
        note.textContent =
          "This sheet is shown as written: calculating it needs µAgent's sheet engine, which is not installed here.";
        document.body.prepend(note);
        return [`the sheet could not be calculated: ${error.message}`];
      })
      .finally(() => delete root.dataset.uagentBusy);
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
  // How long a painted frame is waited for before a page counts as drawn.
  const PAINT_MS = 100;
  // Find: every occurrence of the words marked, the `index`th as the current
  // one; answers how many there are. Letters fold as the app's find folds
  // them (A-Z only); a slide's notes, which are not drawn, are not searched.
  const fold = (text) => text.replace(/[A-Z]/g, (c) => c.toLowerCase());
  const find = (text, index) => {
    const needle = fold(text);
    const ranges = [];
    const walk = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    while (needle && walk.nextNode()) {
      const node = walk.currentNode;
      if (
        node.parentElement.closest("script, style, template") ||
        notes()?.contains(node)
      )
        continue;
      const folded = fold(node.textContent);
      for (
        let at = folded.indexOf(needle);
        at >= 0;
        at = folded.indexOf(needle, at + needle.length)
      ) {
        const range = document.createRange();
        range.setStart(node, at);
        range.setEnd(node, at + needle.length);
        ranges.push(range);
      }
    }
    // A document's are counted by the page they lie on, and the current
    // one is the `index`th of the page shown.
    const from = document.body.getBoundingClientRect().top;
    const pages =
      paper() &&
      ranges.map((range) =>
        tops.findLastIndex(
          (start, at) =>
            !at || start <= range.getBoundingClientRect().top - from + 1,
        ),
      );
    const current = (
      pages ? ranges.filter((_, at) => pages[at] === shown) : ranges
    )[index];
    if (CSS.highlights) {
      CSS.highlights.set("uagent-find", new Highlight(...ranges));
      CSS.highlights.set(
        "uagent-found",
        new Highlight(...(current ? [current] : [])),
      );
    }
    send("find", {
      count: ranges.length,
      counts:
        pages &&
        tops.map((_, at) => pages.filter((page) => page === at).length),
    });
  };
  // Marked in the system's own colours for it: the page's are unknown.
  addEventListener("DOMContentLoaded", () => {
    const marks = document.createElement("style");
    marks.textContent =
      "::highlight(uagent-find){background:Mark;color:MarkText}" +
      "::highlight(uagent-found){background:Highlight;color:HighlightText}";
    document.head.append(marks);
  });
  const height = () => root.scrollHeight;
  checked((problem) => send("error", { message: problem, line: 0 }));
  // What was last asked to be found: a document marks it again on the page
  // it turns to, for the turn and the asking arrive either way round.
  let counted = 0;
  let sought;
  paged = (count) => {
    if (count !== counted) send("pages", { count: (counted = count) });
    if (sought) find(...sought);
  };
  const failed = (message, line) =>
    send("error", {
      message: String(message || "error").slice(0, 300),
      line: line || 0,
    });
  addEventListener("error", (event) => failed(event.message, event.lineno));
  addEventListener("unhandledrejection", (event) =>
    failed(event.reason?.message || event.reason, 0),
  );
  // Drawn: its stylesheets in (or all of it loaded: one may fail), a sheet
  // calculated, and a frame of it painted. A page the browser does not paint
  // (off screen, a hidden tab) is not waited for.
  addEventListener("DOMContentLoaded", async () => {
    await Promise.race([
      Promise.all(
        [...document.querySelectorAll('link[rel="stylesheet"]')].map(
          (link) =>
            link.sheet ||
            new Promise((styled) => link.addEventListener("load", styled)),
        ),
      ),
      new Promise((loaded) => addEventListener("load", loaded)),
    ]);
    await window.uagentReady;
    await Promise.race([
      new Promise((painted) =>
        requestAnimationFrame(() => requestAnimationFrame(painted)),
      ),
      new Promise((late) => setTimeout(late, PAINT_MS)),
    ]);
    send("drawn");
  });
  addEventListener("DOMContentLoaded", () => {
    send("ready", {
      title: document.title,
      artboard: artboard(),
      notes: notes()?.textContent.trim() || "",
      tweaks: tweaks(),
      height: height(),
      // Where its stylesheets are, so the app can read a design system's.
      sheets: [...document.querySelectorAll('link[rel="stylesheet"]')].map(
        (link) => link.href,
      ),
      // A sheet takes the app's look for its grid.
      sheet: !!meta("sheet"),
    });
    let last = 0;
    new ResizeObserver(() => {
      if (height() !== last) send("size", { height: (last = height()) });
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
  // The keys the pane steps with, unless a field has them: a slide or a
  // document's sheet, which has nothing to scroll, gives up the paging keys
  // too (Space only where it presses nothing).
  addEventListener("keydown", (event) => {
    const within = (selector) => event.target.closest?.(selector);
    if (within("input, textarea, select, [contenteditable]")) return;
    const paging =
      stepped() &&
      ["Home", "End", "PageUp", "PageDown", " "].includes(event.key) &&
      !(event.key === " " && within("button, a, summary"));
    if (paging || ["Escape", "ArrowLeft", "ArrowRight"].includes(event.key))
      send("key", { key: event.key });
  });
  // A swipe across a slide or a sheet steps as the arrow keys do.
  let swipe = null;
  addEventListener(
    "touchstart",
    (event) => {
      swipe = event.touches.length === 1 ? event.touches[0] : null;
    },
    { passive: true },
  );
  addEventListener("touchend", (event) => {
    const from = swipe;
    swipe = null;
    if (!from || !stepped()) return;
    const dx = event.changedTouches[0].clientX - from.clientX;
    const dy = event.changedTouches[0].clientY - from.clientY;
    if (Math.abs(dx) > innerWidth / 8 && Math.abs(dx) > 2 * Math.abs(dy))
      send("key", { key: dx < 0 ? "ArrowRight" : "ArrowLeft" });
  });
  addEventListener("message", (event) => {
    if (event.source !== parent || event.data?.uagentDesign !== 1) return;
    if (event.data.type === "select") select(!!event.data.on);
    if (event.data.type === "edit" && paper()) edit(!!event.data.on);
    if (event.data.type === "markdown") send("markdown", { text: markdown() });
    if (event.data.type === "find") {
      sought = [String(event.data.text), Number(event.data.index)];
      if (paper()) turn();
      else find(...sought);
    }
    if (event.data.type === "tweak")
      tweak(String(event.data.name), event.data.value);
  });
})();
