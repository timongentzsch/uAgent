import { markPath } from "../src/mark.ts";
import { chromium } from "@playwright/test";
import { writeFile } from "node:fs/promises";

// Code-native mark; regenerate with the browser already used by UI tests.
const browser = await chromium.launch();
try {
  const page = await browser.newPage({ deviceScaleFactor: 1 });
  await page.setContent(`<style>body{margin:0;background:#000}svg{display:block;width:100vw;height:100vh}</style>
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
      <path fill="#fff" d="${markPath}"/>
    </svg>`);
  for (const [name, size] of [
    ["icon-192.png", 192],
    ["icon-512.png", 512],
    ["icon-maskable.png", 512],
    ["apple-touch-icon.png", 180],
  ]) {
    await page.setViewportSize({ width: size, height: size });
    await writeFile(
      new URL(`../public/${name}`, import.meta.url),
      await page.screenshot(),
    );
  }
} finally {
  await browser.close();
}
