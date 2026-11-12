// A menu the sheet draws itself, at a point of the page; the arrows walk
// it, and Escape or a press elsewhere closes it.

let open;

// `items` are commands `{ label, run }` (with `keys` to show, `checked` for
// one that is on or off, `swatch` for a colour), lists `{ label, items }`
// that open in the menu's place, fields `{ field, value, change }`, which
// say what was typed on Enter, and "-" between groups. `closed` runs once
// it is gone.
export function openMenu({ x, y }, items, closed) {
  open?.();
  const menu = document.createElement("div");
  menu.className = "sheet-menu";
  menu.setAttribute("role", "menu");
  const close = () => {
    menu.remove();
    removeEventListener("pointerdown", outside, true);
    open = undefined;
    closed();
  };
  const outside = (event) => menu.contains(event.target) || close();
  const done = (act) => {
    close();
    act();
  };
  const press = (label, role, act) => {
    const control = document.createElement("button");
    Object.assign(control, { type: "button", textContent: label });
    control.setAttribute("role", role);
    control.addEventListener("click", act);
    return control;
  };
  // A list shown in the menu's place leads back to the one it came from.
  const show = (list, back) => {
    const drawn = list.map((item) => {
      if (item === "-") return document.createElement("hr");
      if (item.items)
        return Object.assign(
          press(item.label, "menuitem", () =>
            show(item.items, {
              label: item.label,
              show: () => show(list, back),
            }),
          ),
          { ariaHasPopup: "menu" },
        );
      if (!item.field) {
        const role =
          item.checked === undefined ? "menuitem" : "menuitemcheckbox";
        const control = press(item.label, role, () => done(item.run));
        if (item.checked !== undefined) control.ariaChecked = !!item.checked;
        if (item.keys) control.dataset.keys = item.keys;
        if (item.swatch) control.dataset.swatch = item.swatch;
        return control;
      }
      const control = Object.assign(document.createElement("input"), {
        type: "search",
        placeholder: item.field,
        ariaLabel: item.field,
        value: item.value,
      });
      control.addEventListener("keydown", (event) => {
        if (event.key === "Enter") done(() => item.change(control.value));
      });
      return control;
    });
    if (back)
      drawn.unshift(
        Object.assign(press(back.label, "menuitem", back.show), {
          className: "sheet-menu-back",
        }),
      );
    menu.replaceChildren(...drawn);
    menu.style.left = `${Math.max(0, Math.min(x, innerWidth - menu.offsetWidth))}px`;
    menu.style.top = `${Math.max(0, Math.min(y, innerHeight - menu.offsetHeight))}px`;
    menu.querySelector("button, input").focus();
  };
  menu.addEventListener("keydown", (event) => {
    const by = { ArrowDown: 1, ArrowUp: -1 }[event.key];
    const all = [...menu.querySelectorAll("button, input")];
    if (by)
      all[
        (all.indexOf(document.activeElement) + by + all.length) % all.length
      ].focus();
    else if (event.key === "Escape" || event.key === "Tab") close();
    else return;
    event.preventDefault();
  });
  document.body.append(menu);
  addEventListener("pointerdown", outside, true);
  open = close;
  show(items);
}
