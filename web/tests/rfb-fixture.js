// Minimal RFB 3.8 server for exercising the real noVNC client, including its
// negotiated display scale and encoded pointer coordinates. No client mocking.
export async function serveFramebuffer(page) {
  const pointers = [];
  const width = 800,
    height = 500;
  await page.routeWebSocket(/\/api\/browser\/viewer/, (socket) => {
    let stage = 0,
      pending = Buffer.alloc(0),
      painted = false;
    socket.send(Buffer.from("RFB 003.008\n"));
    socket.onMessage((message) => {
      pending = Buffer.concat([pending, Buffer.from(message)]);
      while (pending.length) {
        if (stage === 0) {
          if (pending.length < 12) return;
          pending = pending.subarray(12);
          socket.send(Buffer.from([1, 1]));
          stage++;
        } else if (stage === 1) {
          pending = pending.subarray(1);
          socket.send(Buffer.alloc(4));
          stage++;
        } else if (stage === 2) {
          pending = pending.subarray(1);
          const init = Buffer.alloc(28);
          init.writeUInt16BE(width, 0);
          init.writeUInt16BE(height, 2);
          init.set([32, 24, 0, 1], 4);
          for (const offset of [8, 10, 12]) init.writeUInt16BE(255, offset);
          init.set([16, 8, 0], 14);
          init.writeUInt32BE(4, 20);
          init.write("Test", 24);
          socket.send(init);
          stage++;
        } else {
          const kind = pending[0];
          if (kind === 2 && pending.length < 4) return;
          const length =
            kind === 0
              ? 20
              : kind === 2
                ? 4 + 4 * pending.readUInt16BE(2)
                : kind === 3
                  ? 10
                  : kind === 4
                    ? 8
                    : kind === 5
                      ? 6
                      : 0;
          if (!length) throw new Error(`Unexpected client RFB message ${kind}`);
          if (pending.length < length) return;
          if (kind === 5)
            pointers.push({
              buttons: pending[1],
              x: pending.readUInt16BE(2),
              y: pending.readUInt16BE(4),
            });
          if (kind === 3 && !painted) {
            painted = true;
            const frame = Buffer.alloc(16 + width * height * 4, 0xdd);
            frame.fill(0, 0, 16);
            frame.writeUInt16BE(1, 2);
            frame.writeUInt16BE(width, 8);
            frame.writeUInt16BE(height, 10);
            socket.send(frame);
          }
          pending = pending.subarray(length);
        }
      }
    });
  });
  return {
    pointers,
    width,
    height,
    prepare: () =>
      page.evaluate(() => {
        // noVNC introspects own/direct-prototype properties. Playwright's routed
        // socket inherits an extra layer. Forward its actual transport unchanged.
        WebSocket = new Proxy(WebSocket, {
          construct(Target, args) {
            const socket = new Target(...args);
            const channel = {
              send: socket.send.bind(socket),
              close: socket.close.bind(socket),
            };
            for (const name of [
              "binaryType",
              "onerror",
              "onmessage",
              "onopen",
              "onclose",
              "protocol",
              "readyState",
              "bufferedAmount",
            ]) {
              Object.defineProperty(channel, name, {
                enumerable: true,
                get: () => socket[name],
                set: (value) => {
                  socket[name] = value;
                },
              });
            }
            return channel;
          },
        });
      }),
  };
}

export const touch = (locator, type, pointerId, x, y) =>
  locator.dispatchEvent(type, {
    pointerId,
    pointerType: "touch",
    clientX: x,
    clientY: y,
  });
