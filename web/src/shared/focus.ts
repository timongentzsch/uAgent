// Programmatic focus in menus and dialogs can inherit :focus-visible from
// the previously focused control. Track actual input once for every surface;
// focus restoration stays intact, while pointer navigation stays unringed.
addEventListener(
  "pointerdown",
  () => {
    document.documentElement.dataset.input = "pointer";
  },
  true,
);
addEventListener(
  "keydown",
  (event) => {
    if (
      event.metaKey ||
      event.ctrlKey ||
      event.altKey ||
      ["Shift", "Control", "Alt", "Meta"].includes(event.key)
    )
      return;
    document.documentElement.dataset.input = "keyboard";
  },
  true,
);
