// The host-generated deck contains one opaque frame per printable slide.
// Each sends the same “drawn” message Files receives after fonts, fetched
// data and its own rendering promise settle. The host's PDF capture awaits
// this promise. A browser print window also opens its native print dialog.
(() => {
  const interactive = location.hash === "#print";
  const drawn = new Set();
  let slides;
  let button;
  let timeout;
  let autoPrint = interactive;

  window.uagentReady = new Promise((resolve) => {
    const check = () => {
      if (
        !slides ||
        drawn.size < slides.length ||
        slides.some((frame) => !drawn.has(frame.contentWindow))
      )
        return;
      clearTimeout(timeout);
      removeEventListener("message", mark);
      if (button) {
        button.disabled = !slides.length;
        button.textContent = slides.length
          ? "Print deck"
          : "No slides to print";
      }
      resolve();
      if (autoPrint && slides.length) print();
    };
    const mark = (event) => {
      if (
        event.origin !== "null" ||
        event.source?.parent !== window ||
        event.data?.uagentDesign !== 1 ||
        event.data.type !== "drawn"
      )
        return;
      drawn.add(event.source);
      check();
    };
    addEventListener("message", mark);
    addEventListener("DOMContentLoaded", () => {
      slides = [...document.querySelectorAll("iframe")];
      if (interactive) {
        document.title = decodeURIComponent(
          location.pathname.split("/").slice(3).filter(Boolean).at(-1) ||
            "Deck",
        );
        button = document.createElement("button");
        button.disabled = true;
        button.textContent = "Preparing slides…";
        button.onclick = () => {
          autoPrint = false;
          print();
        };
        const style = document.createElement("style");
        style.textContent = "@media print {body > button {display:none}}";
        document.head.append(style);
        document.body.prepend(button);
        // An unavailable frame must not silently print a partial deck.
        timeout = setTimeout(() => {
          autoPrint = false;
          button.disabled = false;
          button.textContent = "Some slides are still loading. Print anyway";
        }, 10000);
      }
      check();
    });
  });
})();
