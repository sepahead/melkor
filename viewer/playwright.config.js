import { defineConfig, devices } from "@playwright/test";

// Render tests for the SparkJS splat viewer. WebGL2 must work in headless Chromium;
// the swiftshader flags let it fall back to software GL when no GPU is exposed.
export default defineConfig({
  testDir: "./tests",
  outputDir: "./test-results",
  fullyParallel: false,
  workers: 1,
  timeout: 150_000,
  expect: { timeout: 15_000 },
  reporter: [["list"]],
  use: {
    baseURL: "http://127.0.0.1:8771",
    viewport: { width: 1280, height: 800 },
    headless: true,
    launchOptions: {
      args: ["--ignore-gpu-blocklist", "--enable-unsafe-swiftshader", "--use-angle=default"],
    },
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
  webServer: {
    command: "QUIET=1 bun serve.js",
    url: "http://127.0.0.1:8771/index.html",
    reuseExistingServer: true,
    timeout: 30_000,
  },
});
