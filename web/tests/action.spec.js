import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";

test.beforeEach(async ({ page }) => {
  await page.route(`${DEV_SERVER_URL}/action-test`, (route) =>
    route.fulfill({
      contentType: "text/html",
      body: "<!doctype html><title>Action test</title>",
    }),
  );
  await page.goto(`${DEV_SERVER_URL}/action-test`);
});

test("actions admit one caller before render and recover after failure", async ({
  page,
}) => {
  // No app or host is needed; Vite supplies the real hook and Preact runtime.
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
    await page.evaluate(() =>
      window.probe.action.run(async () => {
        throw new Error("duplicate admitted after render");
      }),
    ),
  ).toBe(false);
  expect(await page.evaluate(() => window.probe.errors)).toEqual([]);
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

test("a failed concurrent read keeps another read busy and preserves its error", async ({
  page,
}) => {
  await page.evaluate(async () => {
    const { mountAction } = await import("/tests/action-harness.js");
    const action = mountAction({ concurrent: true });
    let finish;
    const pending = action.run(
      () =>
        new Promise((resolve) => {
          finish = resolve;
        }),
    );
    const failed = await action.run(async () => {
      throw new Error("read failed");
    });
    window.probe = { action, finish, pending, failed };
  });
  expect(await page.evaluate(() => window.probe.failed)).toBe(false);
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state()))
    .toEqual({ busy: true, error: "read failed" });
  expect(
    await page.evaluate(async () => {
      window.probe.finish();
      return window.probe.pending;
    }),
  ).toBe(true);
  await expect
    .poll(() => page.evaluate(() => window.probe.action.state()))
    .toEqual({ busy: false, error: "read failed" });
  await page.evaluate(() => window.probe.action.close());
});

test("uploads serialize per conversation before render and recover from failure", async ({
  page,
}) => {
  const requests = [];
  await page.route("**/api/sessions/*/attachments?*", (route) => {
    requests.push(route);
  });
  const duplicate = await page.evaluate(async () => {
    const { mountUploads } = await import("/tests/action-harness.js");
    const uploads = await mountUploads();
    const first = uploads.upload();
    const duplicate = uploads.upload();
    window.probe = { uploads, first, duplicate };
    // Do not await the first request: it is deliberately held by the test.
    return Promise.race([duplicate, Promise.resolve("pending")]);
  });
  expect(duplicate).toBe(false);
  await expect.poll(() => requests.length).toBe(1);
  await page.evaluate(() => window.probe.uploads.select("second"));
  await expect
    .poll(() => page.evaluate(() => window.probe.uploads.state().selected))
    .toBe("second");
  await page.evaluate(() => {
    window.probe.second = window.probe.uploads.upload();
  });
  await expect.poll(() => requests.length).toBe(2);
  await requests[1].fulfill({ json: { id: "second-asset", name: "note.txt" } });
  expect(await page.evaluate(() => window.probe.second)).toBe(true);
  await requests[0].fulfill({ status: 500, json: { error: "upload failed" } });
  expect(await page.evaluate(() => window.probe.first)).toBe(false);
  await page.evaluate(() => window.probe.uploads.select("first"));
  await expect
    .poll(() => page.evaluate(() => window.probe.uploads.state()))
    .toMatchObject({
      selected: "first",
      drafts: {
        first: { files: [] },
        second: { files: [{ id: "second-asset" }] },
      },
      errors: ["upload failed"],
    });
  await page.evaluate(() => {
    window.probe.retry = window.probe.uploads.upload();
  });
  await expect.poll(() => requests.length).toBe(3);
  await requests[2].fulfill({ json: { id: "retry-asset", name: "note.txt" } });
  expect(await page.evaluate(() => window.probe.retry)).toBe(true);
  await expect
    .poll(() =>
      page.evaluate(() => window.probe.uploads.state().drafts.first.files),
    )
    .toEqual([{ id: "retry-asset", name: "note.txt" }]);
  await page.evaluate(() => window.probe.uploads.close());
});
