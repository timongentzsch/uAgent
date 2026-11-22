// A menu the sheet draws itself, at a point of the page; the arrows walk
// it, and Escape, a press elsewhere or the page losing the focus (a press
// in the app around it, where the focus is then left) closes it.

let open;

// `items` are commands `{ label, run }` (with `keys` to show), fields
// `{ field, value, change }`, which say what was typed on Enter, and "-"
// between groups. `closed` runs once it is gone.
export function openMenu({ x, y }, items, closed) {
  open?.();
  const menu = document.createElement("div");
  menu.className = "sheet-menu";
  menu.setAttribute("role", "menu");
  const leave = () => {
    menu.remove();
    removeEventListener("pointerdown", outside, true);
    removeEventListener("blur", leave);
    open = undefined;
  };
  const close = () => {
    leave();
    closed();
  };
  const outside = (event) => menu.contains(event.target) || close();
  const done = (act) => {
    close();
    act();
  };
  const press = (label, act) => {
    const control = document.createElement("button");
    Object.assign(control, { type: "button", textContent: label });
    control.setAttribute("role", "menuitem");
    control.addEventListener("click", act);
    return control;
  };
  const show = (list) => {
    const drawn = list.map((item) => {
      if (item === "-") return document.createElement("hr");
      if (!item.field) {
        const control = press(item.label, () => done(item.run));
        if (item.keys) control.dataset.keys = item.keys;
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
  addEventListener("blur", leave);
  open = close;
  show(items);
}
