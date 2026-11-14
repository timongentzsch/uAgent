// The dev server's port comes from this checkout's path, so runs in two
// checkouts seldom meet. A server is never reused: it may serve another tree.
const here = new URL("..", import.meta.url).pathname;
let hash = 0;
for (const letter of here) hash = (hash * 31 + letter.charCodeAt(0)) % 1000;
export const devServerUrl = `http://127.0.0.1:${5174 + hash}`;
