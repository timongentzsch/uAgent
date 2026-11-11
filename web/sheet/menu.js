// A menu the sheet draws itself, at a point of the page; the arrows walk
// it, and Escape or a press elsewhere closes it.

let open;

// `items` are commands `{ label, run }` and fields `{ field, value,
// change }`, which say what was typed on Enter; `closed` runs once it is gone.
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
  for (const item of items) {
    const control = document.createElement(item.field ? "input" : "button");
    if (item.field) {
      Object.assign(control, {
        type: "search",
        placeholder: item.field,
        ariaLabel: item.field,
        value: item.value,
      });
      control.addEventListener("keydown", (event) => {
        if (event.key === "Enter") done(() => item.change(control.value));
      });
    } else {
      Object.assign(control, { type: "button", textContent: item.label });
      control.setAttribute("role", "menuitem");
      control.addEventListener("click", () => done(item.run));
    }
    menu.append(control);
  }
  menu.addEventListener("keydown", (event) => {
    const by = { ArrowDown: 1, ArrowUp: -1 }[event.key];
    const all = [...menu.children];
    if (by)
      all[
        (all.indexOf(document.activeElement) + by + all.length) % all.length
      ].focus();
    else if (event.key === "Escape" || event.key === "Tab") close();
    else return;
    event.preventDefault();
  });
  document.body.append(menu);
  menu.style.left = `${Math.max(0, Math.min(x, innerWidth - menu.offsetWidth))}px`;
  menu.style.top = `${Math.max(0, Math.min(y, innerHeight - menu.offsetHeight))}px`;
  addEventListener("pointerdown", outside, true);
  open = close;
  menu.firstElementChild.focus();
}
