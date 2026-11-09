import { test } from "node:test";
import assert from "node:assert/strict";
import { bindZoomWheel } from "../src/shared/zoom.ts";

test("wheel zoom uses local coordinates, preserves scrolling and removes its listener", () => {
  let listener;
  const element = {
    getBoundingClientRect: () => ({ left: 20, top: 30 }),
    addEventListener(type, fn, options) {
      assert.equal(type, "wheel");
      assert.equal(options.passive, false);
      listener = fn;
    },
    removeEventListener(type, fn) {
      assert.equal(type, "wheel");
      assert.equal(fn, listener);
      listener = undefined;
    },
  };
  const zooms = [];
  let prevented = 0;
  const event = {
    clientX: 45,
    clientY: 70,
    deltaX: 2,
    deltaY: 10,
    preventDefault() {
      prevented++;
    },
  };
  const release = bindZoomWheel(element, (...args) => zooms.push(args));
  listener(event);
  assert.equal(prevented, 0);
  for (const key of ["ctrlKey", "metaKey"]) listener({ ...event, [key]: true });
  assert.deepEqual(zooms, Array(2).fill([Math.exp(-0.1), { x: 25, y: 40 }]));
  assert.equal(prevented, 2);
  release();
  assert.equal(listener, undefined);
  let accept = false;
  const unbind = bindZoomWheel(
    element,
    () => assert.fail("ordinary wheel zoomed"),
    (dx, dy) => {
      assert.deepEqual([dx, dy], [-2, -10]);
      return accept;
    },
  );
  listener(event);
  assert.equal(prevented, 2);
  accept = true;
  listener(event);
  assert.equal(prevented, 3);
  unbind();
});
