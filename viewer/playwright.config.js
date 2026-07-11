import { defineConfig, devices } from "@playwright/test";

const IS_CI = /^(1|true)$/i.test(process.env.CI ?? "");
const TEST_PORT = Number(process.env.VIEWER_TEST_PORT ?? 8771);
const viewport = IS_CI ? { width: 800, height: 500 } : { width: 1280, height: 800 };
const webglArgs = IS_CI
  ? ["--ignore-gpu-blocklist", "--enable-unsafe-swiftshader", "--use-gl=angle", "--use-angle=swiftshader"]
  : ["--ignore-gpu-blocklist", "--enable-unsafe-swiftshader", "--use-angle=default"];

// Render tests for the SparkJS splat viewer. WebGL2 must work in headless Chromium;
// CI selects Chromium's documented SwANGLE driver explicitly so GPU-less runners
// exercise one deterministic software renderer instead of an implicit fallback.
export default defineConfig({
  testDir: "./tests",
  outputDir: "./test-results",
  fullyParallel: false,
  workers: 1,
  timeout: 150_000,
  expect: { timeout: 15_000 },
  reporter: [["list"]],
  use: {
    baseURL: `http://127.0.0.1:${TEST_PORT}`,
    viewport,
    headless: true,
    screenshot: "only-on-failure",
    launchOptions: {
      args: webglArgs,
    },
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"], viewport } }],
  webServer: {
    command: `PORT=${TEST_PORT} QUIET=1 bun serve.js`,
    url: `http://127.0.0.1:${TEST_PORT}/index.html`,
    reuseExistingServer: !IS_CI,
    timeout: 30_000,
  },
});
