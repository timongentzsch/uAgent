// Runs first in every page the Files pane previews (the host inlines it): by
// postMessage it says what the page is and what fails in it, and takes tweaks.
(() => {
  const root = document.documentElement;
  const meta = (name) =>
    document.querySelector(`meta[name="${name}"]`)?.content.trim() || "";
  // A document's print line: "flow", then in any order its paper (a name or
  // WxHmm), "landscape", and its margins: a preset, or one to four lengths.
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
  const MM = { mm: 1, cm: 10, in: 25.4, pt: 25.4 / 72, px: 25.4 / 96 };
  // In millimetres, the margins from the top clockwise; `wrong` is what of
  // the line was not understood.
  const paper = () => {
    const [flow, ...said] = meta("print").toLowerCase().split(/\s+/);
    if (flow !== "flow") return null;
    let size = PAPER.a4;
    let turned = false;
    let preset = MARGINS.normal;
    const lengths = [];
    const wrong = [];
    for (const word of said) {
      const custom = /^(\d+(?:\.\d+)?)x(\d+(?:\.\d+)?)mm$/.exec(word);
      const length = /^(\d+(?:\.\d+)?)(mm|cm|in|pt|px)$/.exec(word);
      if (custom && custom[1] > 0 && custom[2] > 0)
        size = [+custom[1], +custom[2]];
      else if (Object.hasOwn(PAPER, word)) size = PAPER[word];
      else if (word === "landscape") turned = true;
      else if (Object.hasOwn(MARGINS, word)) preset = MARGINS[word];
      else if (length && lengths.length < 4)
        lengths.push(length[1] * MM[length[2]]);
      else wrong.push(`"${word}" in the print line is not understood`);
    }
    const [w, h] = turned ? [size[1], size[0]] : size;
    let [top, right = top, bottom = top, left = right] = lengths.length
      ? lengths
      : preset;
    // Margins with no room between them are none.
    if (left + right >= w || top + bottom >= h) {
      wrong.push(`the margins leave no room on the ${w}x${h}mm paper`);
      top = right = bottom = left = 0;
    }
    return { w, h, margin: [top, right, bottom, left], wrong };
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
  // A document's blocks that do not fit: wider than the text, for which
  // print shrinks the whole page, or unbreakable and taller than a page.
  const misfits = () => {
    const high = pageHeight();
    const wide = document.body.clientWidth;
    const found = [];
    for (const block of document.querySelectorAll(
      "figure, table, blockquote, pre, img, svg",
    )) {
      if (block.parentElement.closest("figure, table, svg")) continue;
      // Of all its parts: one that is too tall is cut over several pages.
      const boxes = [...block.getClientRects()];
      const across = Math.round(
        Math.max(block.scrollWidth || 0, ...boxes.map((box) => box.width)),
      );
      const down = Math.round(boxes.reduce((sum, box) => sum + box.height, 0));
      if (across > wide + 1)
        found.push(
          `a ${block.localName} is ${across}px wide, more than the page's text (${wide}px): wrap or shrink it, or print shrinks the whole page to fit it`,
        );
      if (
        down > high + 1 &&
        (block.matches("img, svg") ||
          getComputedStyle(block).breakInside === "avoid")
      )
        found.push(
          `a ${block.localName} is ${down}px tall, more than a page (${Math.round(high)}px): shrink it or split it`,
        );
    }
    return found;
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
    if (paper()) found.push(...paper().wrong, ...misfits());
    return found;
  };
  const checked = (report) =>
    addEventListener("load", async () => {
      await document.fonts.ready;
      // A sheet's cells in error, once it is calculated.
      const cells = (await window.uagentReady) ?? [];
      [...problems(), ...cells].forEach(report);
    });

  // A document on screen is one sheet: its body is columns the size of a
  // page's text, broken as the browser breaks pages, moved and cut to one.
  let pages = 1;
  let shown = 0;
  let paged;
  let went;
  // The page a box of the body lies on, from 0.
  const pageOf = (box) => {
    const { w, margin } = paper();
    const from = document.body.getBoundingClientRect().left;
    const across = (box.left - from) / PX_PER_MM + margin[3];
    return Math.floor(across / w + 0.01);
  };
  // As many pages as the body's parts reach: what runs out sideways adds
  // none. Then the page the address names ("#3", or the id of something on
  // it); one that names nothing leaves the page shown.
  const turn = () => {
    const range = document.createRange();
    const ends = [...document.body.childNodes].map((node) => {
      range.selectNode(node);
      const boxes = range.getClientRects();
      return boxes.length ? pageOf(boxes[boxes.length - 1]) : 0;
    });
    pages = 1 + Math.max(0, ...ends);
    const said = decodeURIComponent(location.hash.slice(1));
    const named = /\D/.test(said) && document.getElementById(said);
    if (named) return to(named.getBoundingClientRect());
    shown = Math.min(pages, Math.max(1, Math.floor(said) || shown + 1)) - 1;
    root.style.setProperty("--uagent-page", shown);
    paged?.(pages);
  };
  // To the page of something the page itself went to: a link, the caret.
  const to = (box) => {
    const at = pageOf(box) + 1;
    location.replace(`#${at}`);
    went?.(at);
  };
  addEventListener("DOMContentLoaded", () => {
    const format = paper();
    if (!format) return;
    const { w, h, margin } = format;
    const [top, right, bottom, left] = margin;
    const move = `calc(var(--uagent-page,0)*${w}mm)`;
    const style = document.createElement("style");
    style.textContent =
      `@page{size:${w}mm ${h}mm;margin:${margin.map((side) => `${side}mm`).join(" ")}}` +
      `.title-page{box-sizing:border-box;height:${h - top - bottom}mm!important;min-height:0!important}:where(.title-page){display:flow-root}` +
      `@media screen{html{width:${w}mm;height:${h}mm;overflow:hidden}` +
      `body{position:absolute;top:${top}mm;left:${left}mm;width:${w - left - right}mm;height:${h - top - bottom}mm;` +
      `margin:0!important;padding:0!important;columns:1;column-gap:${left + right}mm;column-fill:auto;` +
      `translate:calc(${move}*-1);` +
      `clip-path:inset(-${top}mm calc(${move}*-1 - ${right}mm) -${bottom}mm calc(${move} - ${left}mm))}` +
      // What is wider than the text is cut there, or it shows on the next page.
      "body>*{overflow-x:clip}" +
      "[data-uagent-before]{break-before:column!important}[data-uagent-after]{break-after:column!important}}";
    // After the page's own, which it overrules.
    document.head.append(style);
    // A page break is a column's here, which no rule of the page's says.
    const forced = ["page", "left", "right", "recto", "verso"];
    const breaks = () => {
      for (const part of document.body.querySelectorAll("*")) {
        const { breakBefore, breakAfter } = getComputedStyle(part);
        if (forced.includes(breakBefore)) part.dataset.uagentBefore = "";
        if (forced.includes(breakAfter)) part.dataset.uagentAfter = "";
      }
      turn();
    };
    breaks();
    addEventListener("load", breaks);
    // Whatever changes how far the text runs changes a block's size.
    const sized = new ResizeObserver(turn);
    for (const block of document.body.children) sized.observe(block);
    addEventListener("hashchange", turn);
    // A link to a place in the document turns to its page ("#", the first);
    // one to nowhere does nothing.
    addEventListener("click", (event) => {
      const link = event.target.closest?.('a[href^="#"]');
      if (!link || event.defaultPrevented) return;
      event.preventDefault();
      const name = decodeURIComponent(link.getAttribute("href").slice(1));
      const place = name ? document.getElementById(name) : document.body;
      if (place) to(place.getBoundingClientRect());
    });
    // What would scroll the sheet (the caret, the focus) turns the page.
    addEventListener("scroll", () => {
      if (!scrollX && !scrollY) return;
      scrollTo(0, 0);
      const active = document.activeElement;
      const on =
        active.isContentEditable && getSelection().rangeCount
          ? getSelection().getRangeAt(0)
          : active;
      if (on !== document.body)
        to(on.getClientRects()[0] ?? on.getBoundingClientRect());
    });
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
    // Alone, a document turns its own pages by the keys the pane steps with.
    addEventListener("keydown", (event) => {
      const by = {
        ArrowRight: 1,
        PageDown: 1,
        ArrowLeft: -1,
        PageUp: -1,
        Home: -pages,
        End: pages,
      }[event.key];
      // With a modifier the key is the browser's (Back, a tab's end).
      const held = event.metaKey || event.ctrlKey || event.altKey;
      if (!by || held || !paper()) return;
      if (event.target.closest?.("input, textarea, select")) return;
      event.preventDefault();
      location.replace(`#${Math.min(pages, Math.max(1, shown + 1 + by))}`);
    });
    if (location.hash !== "#print" && location.hash !== "#paper") return;
    addEventListener("DOMContentLoaded", paperStyle);
    if (location.hash === "#print")
      addEventListener("load", async () => {
        await document.fonts.ready;
        print();
      });
    return;
  }

  // A document's or a slide's text blocks, in order: what a person edits in
  // place.
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
  // Edit: text blocks take typing; a block is reported as it changes, with
  // its place and its text when Edit went on, which the app checks against
  // the file.
  const before = new WeakMap();
  const report = (block, more) => {
    // Without the marks put on its parts here.
    const clean = block.cloneNode(true);
    for (const part of clean.querySelectorAll("*"))
      for (const mark of ["data-uagent-before", "data-uagent-after"])
        part.removeAttribute(mark);
    send("edit", {
      index: blocks().indexOf(block),
      before: before.get(block),
      html: clean.innerHTML,
      ...more,
    });
  };
  const changed = (event) => {
    const block = event.target.closest?.("[contenteditable]");
    if (block) report(block);
  };
  // A block left: the app shows its legend the file as it now is.
  const left = (event) =>
    event.target.closest?.("[contenteditable]") && send("left");
  // Where the selection starts, and the block being edited it is in.
  const startOf = () => {
    const node = getSelection().anchorNode;
    return node?.nodeType === Node.TEXT_NODE ? node.parentElement : node;
  };
  const editing = () => startOf()?.closest?.("[contenteditable]") || null;
  // What a block can be turned into another of: a paragraph, a heading or an
  // item of a plain list, named by its list.
  const kindOf = (block) =>
    block.matches("p, h2, h3")
      ? block.localName
      : block.matches(":is(ul, ol) > li") && !block.querySelector("ul, ol")
        ? block.parentElement.localName
        : "";
  // What the app's tools show of the selection, said as it changes: the
  // block's kind, whether anything is selected, its marks (none to make
  // where the block's own style is the mark) and the link it starts in.
  let told = "";
  const caret = () => {
    const block = editing();
    const own = block && getComputedStyle(block);
    const state = block
      ? {
          kind: kindOf(block),
          selected: !getSelection().isCollapsed,
          bold:
            own.fontWeight < 600 ? document.queryCommandState("bold") : null,
          italic:
            own.fontStyle === "normal"
              ? document.queryCommandState("italic")
              : null,
          href: startOf().closest("a[href]")?.getAttribute("href") || "",
        }
      : {};
    if (told !== (told = JSON.stringify(state))) send("caret", state);
  };
  // A mark or a link on what is selected in the block, made as a rich text
  // field makes them (a link is changed or taken away whole); or the block
  // reported with the kind it is to become.
  const format = ({ mark, href, as }) => {
    const block = editing();
    if (!block) return;
    if (as) return report(block, { as });
    const link = startOf().closest("a[href]");
    if (link && !mark) getSelection().selectAllChildren(link);
    block.setAttribute("contenteditable", "true");
    if (mark) document.execCommand(mark === "italic" ? "italic" : "bold");
    else if (href) document.execCommand("createLink", false, String(href));
    else document.execCommand("unlink");
    block.setAttribute("contenteditable", "plaintext-only");
    block.focus();
    caret();
  };
  // Edit on or off; with `at`, the caret goes to the end of that block.
  const edit = (on, at) => {
    for (const block of blocks())
      if (on) {
        block.setAttribute("contenteditable", "plaintext-only");
        before.set(block, block.textContent);
      } else block.removeAttribute("contenteditable");
    const toggle = on ? addEventListener : removeEventListener;
    toggle("input", changed, true);
    toggle("focusout", left, true);
    toggle.call(document, "selectionchange", caret);
    told = "";
    const block = on && blocks()[at];
    if (!block) return;
    block.focus();
    getSelection().selectAllChildren(block);
    getSelection().collapseToEnd();
  };

  const send = (type, data) =>
    parent.postMessage({ uagentDesign: 1, type, ...data }, "*");
  // How long a painted frame is waited for before a page counts as drawn.
  const PAINT_MS = 100;
  // Find: every occurrence of the words marked, the `index`th as the current
  // one. Letters fold as the app's find does (A-Z); notes are not searched.
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
    // A document's are counted by the page they lie on, those drawn, and
    // the current one is the `index`th of the page shown.
    const lies =
      paper() &&
      ranges.map((range) => {
        const [box] = range.getClientRects();
        return box ? pageOf(box) : -1;
      });
    const current = (
      lies ? ranges.filter((_, at) => lies[at] === shown) : ranges
    )[index];
    if (CSS.highlights) {
      CSS.highlights.set("uagent-find", new Highlight(...ranges));
      CSS.highlights.set(
        "uagent-found",
        new Highlight(...(current ? [current] : [])),
      );
    }
    send("find", {
      counts:
        lies &&
        Array.from(
          { length: pages },
          (_, at) => lies.filter((page) => page === at).length,
        ),
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
  went = (at) => send("page", { at });
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
    if (event.data.type === "edit" && stepped())
      edit(!!event.data.on, event.data.at);
    if (event.data.type === "format") format(event.data);
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
