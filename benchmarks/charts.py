"""Small SVG charts using the web UI's monochrome palette and monospace type."""

from html import escape
from math import ceil

# Matches web/src/shared/styles/{base,tokens}.css. Embedded for standalone SVGs.
STYLE = """
  :root { --bg:#fff; --text:#111; --muted:#555; --line:#ccc; --bar:#767676; }
  @media (prefers-color-scheme:dark) {
    :root { --bg:#000; --text:#f5f5f5; --muted:#a3a3a3; --line:#404040; --bar:#8a8a8a; }
  }
  .bg { fill:var(--bg); }
  text { fill:var(--text); font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
    font-size:13px; font-variant-numeric:tabular-nums; }
  .headline { font-size:22px; font-weight:700; }
  .title { font-size:14px; font-weight:700; }
  .note { font-size:11px; fill:var(--muted); }
  .bar { fill:var(--bar); }
  .bar.primary { fill:var(--text); }
  .rule { stroke:var(--line); }
"""


def text(x, y, value, css="", anchor="start"):
    return f'<text x="{x}" y="{y}" class="{css}" text-anchor="{anchor}">{escape(str(value))}</text>'


def document(title, width, height, body, notes):
    return "\n".join(
        [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img">',
            f"<title>{escape(title)}</title>",
            f"<desc>{escape(' · '.join(notes))}</desc>",
            f"<style>{STYLE}</style>",
            f'<rect class="bg" width="{width}" height="{height}"/>',
            text(24, 40, title, "headline"),
            '<path class="rule" d="M24 58H' + str(width - 24) + '"/>',
            *body,
            *[text(24, height - 18 - 18 * i, note, "note") for i, note in enumerate(notes[::-1])],
            "</svg>\n",
        ]
    )


def bars(title, panels, notes, label_width=110, shared_scale=False):
    """Panels are (title, [(label, value, highlight)], decimal places)."""
    columns, panel_width, gap = 2, 420, 32
    bar_width = panel_width - label_width - 60
    panel_height = 48 + 32 * max(len(rows) for _, rows, _ in panels)
    width = 48 + columns * panel_width + (columns - 1) * gap
    height = 86 + ceil(len(panels) / columns) * panel_height + 18 * len(notes)
    overall_peak = max((v for _, rows, _ in panels for _, v, _ in rows if v is not None), default=1)
    body = []
    for index, (heading, rows, places) in enumerate(panels):
        x, y = 24 + index % columns * (panel_width + gap), 88 + index // columns * panel_height
        body.append(text(x, y, heading, "title"))
        peak = (
            overall_peak
            if shared_scale
            else max((v for _, v, _ in rows if v is not None), default=1)
        )
        for row, (label, value, highlight) in enumerate(rows):
            top = y + 22 + row * 32
            body.append(text(x, top + 14, label))
            if value is None:
                body.append(text(x + label_width, top + 14, "n/a"))
                continue
            length = value / (peak or 1) * bar_width
            body.append(
                f'<rect class="bar{" primary" if highlight else ""}" x="{x + label_width}" '
                f'y="{top}" width="{length:.2f}" height="20"/>'
            )
            body.append(
                text(f"{x + label_width + length + 8:.2f}", top + 14, f"{value:.{places}f}")
            )
    return document(title, width, height, body, notes)
