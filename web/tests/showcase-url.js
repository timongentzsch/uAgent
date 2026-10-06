// The showcase's dev server, on a port taken from this checkout's path, so
// runs in two checkouts seldom meet. When they do the second fails to start
// its server: one is never reused, since it may serve another tree.
const here = new URL("..", import.meta.url).pathname;
let hash = 0;
for (const letter of here) hash = (hash * 31 + letter.charCodeAt(0)) % 1000;
export const showcaseUrl = `http://127.0.0.1:${5174 + hash}/ui.html`;
