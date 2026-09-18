// Runs before the stylesheet and app load; an external file keeps the CSP free of inline JS.
(() => {
  let t;
  try {
    t = localStorage.getItem("uagent-theme");
  } catch {}
  if (t !== "light" && t !== "dark")
    t = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  document.documentElement.dataset.theme = t;
  document.querySelector('meta[name="theme-color"]').content =
    t === "dark" ? "#000000" : "#ffffff";
})();
