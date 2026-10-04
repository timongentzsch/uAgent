// The showcase's dev server, on a port of this checkout's own: with one
// fixed port, a run in another checkout reused this one's server and tested
// the wrong tree.
const here = new URL("..", import.meta.url).pathname;
let hash = 0;
for (const letter of here) hash = (hash * 31 + letter.charCodeAt(0)) % 1000;
export const showcaseUrl = `http://127.0.0.1:${5174 + hash}/ui.html`;
