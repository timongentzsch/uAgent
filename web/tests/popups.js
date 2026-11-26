// The chrome of the top popup: where its title, its Close and its body
// stand. Distances are in CSS pixels from the popup's inner edges.
export const chrome = (page) =>
  page.evaluate(() => {
    const dialog = [...document.querySelectorAll("dialog[open]")].at(-1);
    const style = getComputedStyle(dialog);
    const outer = dialog.getBoundingClientRect();
    const inner = {
      left: outer.left + Number.parseFloat(style.borderLeftWidth),
      right: outer.right - Number.parseFloat(style.borderRightWidth),
      top: outer.top + Number.parseFloat(style.borderTopWidth),
    };
    const header = dialog.querySelector("header");
    const head = header.getBoundingClientRect();
    const line = Number.parseFloat(getComputedStyle(header).borderBottomWidth);
    const title = document.createRange();
    title.selectNodeContents(header.querySelector("h2"));
    const text = title.getBoundingClientRect();
    const close = header.querySelector('button[aria-label^="Close"]');
    const button = close.getBoundingClientRect();
    const glyph = close.querySelector("svg").getBoundingClientRect();
    const body = dialog.querySelector(":scope > .dialog-body");
    const pad = body && getComputedStyle(body);
    const round = (value) => Math.round(value * 10) / 10;
    return {
      title: round(text.left - inner.left),
      glyph: {
        right: round(inner.right - glyph.right),
        top: round(glyph.top - head.top),
        bottom: round(head.bottom - line - glyph.bottom),
      },
      header: round(head.height - line),
      headerTop: round(head.top),
      // Each control on the bar: its name, and its distance to the bar's
      // top and bottom.
      controls: [...header.querySelectorAll("button, a")].map((node) => {
        const at = node.getBoundingClientRect();
        return [
          node.getAttribute("aria-label") || node.textContent.trim(),
          round(at.top - head.top),
          round(head.bottom - line - at.bottom),
        ];
      }),
      close: {
        width: round(button.width),
        height: round(button.height),
        right: round(inner.right - button.right),
        top: round(button.top - inner.top),
      },
      // Room kept for a scrollbar beside a body that does not scroll.
      gutter:
        body && body.scrollHeight <= body.clientHeight
          ? body.offsetWidth - body.clientWidth
          : 0,
      body: pad && [
        pad.paddingTop,
        pad.paddingRight,
        pad.paddingBottom,
        pad.paddingLeft,
      ],
      outside:
        outer.left < -0.5 ||
        outer.top < -0.5 ||
        outer.right > innerWidth + 0.5 ||
        outer.bottom > innerHeight + 0.5,
    };
  });
