import { defineConfig } from "vite";
import { VitePWA } from "vite-plugin-pwa";
export default defineConfig({
  esbuild: { jsx: "automatic", jsxImportSource: "preact" },
  plugins: [
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
        globPatterns: ["**/*.{html,js,css,woff2,png,webmanifest}"],
        maximumFileSizeToCacheInBytes: 2 * 1024 * 1024,
      },
    }),
  ],
  build: { target: "es2022", cssCodeSplit: true, assetsInlineLimit: 0 },
});
