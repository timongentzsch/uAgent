import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";

test("actions admit one caller before render and recover after failure", async ({
  page,
}) => {
  // No app or host is needed; Vite supplies the real hook and Preact runtime.
  await page.route(`${DEV_SERVER_URL}/action-test`, (route) =>
    route.fulfill({
      contentType: "text/html",
      body: "<!doctype html><title>Action test</title>",
    }),
  );
  await page.goto(`${DEV_SERVER_URL}/action-test`);
  const admitted = await page.evaluate(async () => {
    const { mountAction } = await import("/tests/action-harness.js");
    const errors = [];
    const action = mountAction({
      report: (error) => errors.push(error.message),
    });
    const other = mountAction();
    let finish;
    let calls = 0;
    const first = action.run(() => {
      calls++;
      return new Promise((done) => {
        finish = done;
      });
    });
    const duplicate = await action.run(async () => {
      calls++;
    });
    const independent = await other.run(async () => {});
    window.probe = { action, other, errors, first, finish };
    return { calls, duplicate, independent };
  });
  expect(admitted).toEqual({ calls: 1, duplicate: false, independent: true });
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state()))
    .toEqual({ busy: true, error: null });
  expect(
    await page.evaluate(async () => {
      window.probe.finish();
      return window.probe.first;
    }),
  ).toBe(true);
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state().busy))
    .toBe(false);
  expect(
    await page.evaluate(() =>
      window.probe.action.run(() => {
        throw new Error("refused");
      }),
    ),
  ).toBe(false);
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state()))
    .toEqual({ busy: false, error: "refused" });
  expect(await page.evaluate(() => window.probe.errors)).toEqual(["refused"]);
  expect(
    await page.evaluate(() => window.probe.action.run(async () => {})),
  ).toBe(true);
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state()))
    .toEqual({ busy: false, error: null });
  await page.evaluate(() => {
    window.probe.action.close();
    window.probe.other.close();
  });
});

test("concurrent reads remain busy until both out-of-order answers settle", async ({
  page,
}) => {
  await page.route(`${DEV_SERVER_URL}/action-test`, (route) =>
    route.fulfill({
      contentType: "text/html",
      body: "<!doctype html><title>Action test</title>",
    }),
  );
  await page.goto(`${DEV_SERVER_URL}/action-test`);
  await page.evaluate(async () => {
    const { mountAction } = await import("/tests/action-harness.js");
    const action = mountAction({ concurrent: true });
    let firstDone, secondDone;
    const first = action.run(
      () =>
        new Promise((done) => {
          firstDone = done;
        }),
    );
    const second = action.run(
      () =>
        new Promise((done) => {
          secondDone = done;
        }),
    );
    window.probe = { action, firstDone, secondDone, first, second };
  });
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state().busy))
    .toBe(true);
  expect(
    await page.evaluate(async () => {
      window.probe.secondDone();
      return window.probe.second;
    }),
  ).toBe(true);
  expect(await page.evaluate(() => window.probe.action.state().busy)).toBe(
    true,
  );
  expect(
    await page.evaluate(async () => {
      window.probe.firstDone();
      return window.probe.first;
    }),
  ).toBe(true);
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state().busy))
    .toBe(false);
  await page.evaluate(() => window.probe.action.close());
});
