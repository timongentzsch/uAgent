import { defineConfig } from "vite";
import { VitePWA } from "vite-plugin-pwa";
import { fileURLToPath } from "node:url";
const optionalAssets = new Set();
const rendererManifest = () => ({
  name: "renderer-assets",
  generateBundle(_options, bundle) {
    const chunks = Object.values(bundle).filter(
      (item) => item.type === "chunk",
    );
    const byFile = new Map(chunks.map((chunk) => [chunk.fileName, chunk]));
    const roots = new Set(
      chunks
        .filter((chunk) =>
          [...chunk.moduleIds].some((id) =>
            /\/(?:diagram\.tsx|math\.ts|highlight\.ts)$/.test(id),
          ),
        )
        .map((chunk) => chunk.fileName),
    );
    const pending = [...roots];
    while (pending.length) {
      const file = pending.pop();
      if (!file || optionalAssets.has(file)) continue;
      optionalAssets.add(file);
      const chunk = byFile.get(file);
      for (const dependency of [
        ...(chunk?.imports || []),
        ...(chunk?.dynamicImports || []),
      ])
        pending.push(dependency);
      for (const asset of chunk?.viteMetadata?.importedAssets || [])
        optionalAssets.add(asset);
      for (const css of chunk?.viteMetadata?.importedCss || [])
        optionalAssets.add(css);
    }
    // Shared shell/conversation dependencies remain core. Traverse every
    // entry dependency while treating renderer roots as lazy boundaries.
    const core = new Set();
    const corePending = chunks
      .filter((chunk) => chunk.isEntry)
      .map((chunk) => chunk.fileName);
    while (corePending.length) {
      const file = corePending.pop();
      if (
        !file ||
        core.has(file) ||
        (roots.has(file) && !byFile.get(file)?.isEntry)
      )
        continue;
      core.add(file);
      const chunk = byFile.get(file);
      for (const dependency of [
        ...(chunk?.imports || []),
        ...(chunk?.dynamicImports || []),
      ])
        corePending.push(dependency);
      for (const asset of chunk?.viteMetadata?.importedAssets || [])
        core.add(asset);
      for (const css of chunk?.viteMetadata?.importedCss || []) core.add(css);
    }
    for (const file of core) optionalAssets.delete(file);
    this.emitFile({
      type: "asset",
      fileName: "renderer-assets.json",
      source: JSON.stringify([...optionalAssets].map((path) => `/${path}`)),
    });
  },
});
export default defineConfig({
  esbuild: { jsx: "automatic", jsxImportSource: "preact" },
  plugins: [
    rendererManifest(),
    {
      name: "modern-math-fonts",
      enforce: "pre",
      transform(code, id) {
        if (!id.endsWith("/katex.min.css")) return;
        // Every supported browser reads WOFF2; embedding TTF and WOFF copies
        // adds native binary bytes without adding a supported device.
        return code.replace(
          /src:([^;}]+)([;}])/g,
          (declaration, sources, end) => {
            const woff2 = sources
              .split(",")
              .find((source) => source.includes(".woff2"));
            return woff2 ? `src:${woff2}${end}` : declaration;
          },
        );
      },
    },
    VitePWA({
      strategies: "injectManifest",
      srcDir: "src",
      filename: "sw.ts",
      injectRegister: false,
      registerType: "prompt",
      manifest: {
        id: "/",
        name: "µAgent",
        short_name: "µAgent",
        start_url: "/",
        scope: "/",
        display: "standalone",
        background_color: "#000000",
        theme_color: "#000000",
        icons: [
          { src: "/icon-192.png", sizes: "192x192", type: "image/png" },
          { src: "/icon-512.png", sizes: "512x512", type: "image/png" },
          {
            src: "/icon-maskable.png",
            sizes: "512x512",
            type: "image/png",
            purpose: "maskable",
          },
        ],
      },
      injectManifest: {
        globPatterns: ["**/*.{html,json,js,css,woff2,png,webmanifest}"],
        maximumFileSizeToCacheInBytes: 2 * 1024 * 1024,
        manifestTransforms: [
          async (entries) => ({
            manifest: entries.filter(
              (entry) => !optionalAssets.has(entry.url.replace(/^\//, "")),
            ),
          }),
        ],
      },
    }),
  ],
  build: {
    manifest: true,
    target: "es2022",
    cssCodeSplit: true,
    assetsInlineLimit: 0,
    rollupOptions: {
      input: {
        index: fileURLToPath(new URL("index.html", import.meta.url)),
        ui: fileURLToPath(new URL("ui.html", import.meta.url)),
      },
    },
  },
});
