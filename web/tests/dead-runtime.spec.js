// @webkit
import { execFileSync } from "node:child_process";
import { test, expect, online, selectProbeModel } from "./fixtures.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

// The runtime serving `host`'s conversation: the host's one session worker.
function runtimeOf(host) {
  const rows = execFileSync("ps", ["-axo", "pid=,ppid=,args="], {
    encoding: "utf8",
  });
  for (const row of rows.split("\n")) {
    const [, pid, parent, args] = /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(row) || [];
    if (parent === String(host.pid) && args.includes("--session-worker"))
      return Number(pid);
  }
  throw new Error("no runtime");
}

// A runtime ended by the system mid-turn (out of memory, a crash): the
// conversation says the turn was interrupted, and the next message starts a
// runtime again and is answered.
test("a message continues a conversation whose runtime died mid-turn", async ({
  page,
  host,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Queue controls probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".message.response")).toContainText(
    "Waiting for guidance",
  );
  process.kill(runtimeOf(host), "SIGKILL");
  await expect(page.getByText("Stopped", { exact: true })).toBeVisible();
  await prompt.fill("Continue after the runtime died");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await expect(page.getByText("session needs activation")).toHaveCount(0);
});
