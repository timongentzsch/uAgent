// Run before the stylesheet and app load; keep the native CSP free of inline JS.
(() => {
  let theme;
  try {
    theme = localStorage.getItem("uagent-theme");
  } catch {}
  if (theme !== "light" && theme !== "dark")
    theme = matchMedia("(prefers-color-scheme: dark)").matches
      ? "dark"
      : "light";
  document.documentElement.dataset.theme = theme;
  document.querySelector('meta[name="theme-color"]').content =
    theme === "dark" ? "#000000" : "#ffffff";
})();
