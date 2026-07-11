import { test, expect } from "@playwright/test";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { mkdirSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const SHOT_DIR = join(__dirname, "..", "screenshots");
const LOCAL_SPLAT = join(__dirname, "..", "public", "splats", "generated", "wave.splat");
const IS_CI = /^(1|true)$/i.test(process.env.CI ?? "");
const FULL_RENDER = !IS_CI || /^(1|true)$/i.test(process.env.VIEWER_FULL_RENDER ?? "");
const DEFAULT_TEST_SCENE = IS_CI ? "wave-static" : null;
const RENDER_CAPTURE_STYLE = `
  #hud, #scenes, #bar, #tl, #overlay, #dropTarget {
    visibility: hidden !important;
  }
`;
mkdirSync(SHOT_DIR, { recursive: true });

// City / area scenes in public/splats/. `min` = sane lower bound on splat count.
// `optional` scenes (the melkor-generated .ply) are skipped when their file is absent.
const SCENES = [
  { id: "snow-street",   fmt: "SPZ",   min: 900_000,   file: "snow-street.spz" },
  { id: "valley",        fmt: "SPZ",   min: 450_000,   file: "valley.spz" },
  { id: "sutro",         fmt: "SOG",   min: 1_900_000, file: "sutro.zip" },
  { id: "wave-static",   fmt: "SPLAT", min: 4_000,     file: "generated/wave.splat" },
  { id: "distant-igloo", fmt: "SPZ",   min: 300_000,   file: "distant-igloo.spz" },
  { id: "igloo-ply",     fmt: "PLY",   min: 300_000,   file: "distant-igloo.ply", optional: true },
];
const VIEWS = ["front", "side", "top", "iso"]; // distinct "camera feeds"

async function served(request, file) {
  const r = await request.fetch(`/public/splats/${file}`, { method: "HEAD" });
  return r.status() === 200;
}

// Sample a compositor screenshot rather than WebGL's transient drawing buffer.
// With preserveDrawingBuffer=false, readPixels can legally observe a cleared
// buffer even while Chromium displays a valid frame. The screenshot measures
// what the user actually sees: coverage, tonal variance, and a spatial hash.
async function pixelStats(page) {
  const png = await page.locator("canvas").screenshot({
    type: "png",
    style: RENDER_CAPTURE_STYLE,
  });
  const dataUrl = `data:image/png;base64,${png.toString("base64")}`;
  return await page.evaluate(async (src) => {
    const image = new Image();
    image.src = src;
    await image.decode();
    const canvas = document.createElement("canvas");
    canvas.width = image.naturalWidth;
    canvas.height = image.naturalHeight;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    context.drawImage(image, 0, 0);
    const buf = context.getImageData(0, 0, canvas.width, canvas.height).data;
    let nonBg = 0, n = 0, sum = 0, sum2 = 0, signature = 2166136261;
    for (let i = 0; i < buf.length; i += 4 * 131) {
      const r = buf[i], gg = buf[i + 1], b = buf[i + 2];
      const lum = 0.299 * r + 0.587 * gg + 0.114 * b;
      sum += lum; sum2 += lum * lum; n++;
      if (Math.abs(r - 11) + Math.abs(gg - 12) + Math.abs(b - 16) > 24) nonBg++;
      signature ^= (r | (gg << 8) | (b << 16)) ^ i;
      signature = Math.imul(signature, 16777619) >>> 0;
    }
    const mean = sum / n;
    return {
      nonBgFrac: nonBg / n,
      lumStd: Math.sqrt(Math.max(sum2 / n - mean * mean, 0)),
      signature: signature.toString(16).padStart(8, "0"),
    };
  }, dataUrl);
}

async function boot(page, initialScene = DEFAULT_TEST_SCENE) {
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  const path = initialScene
    ? `/index.html?scene=${encodeURIComponent(initialScene)}`
    : "/index.html";
  await page.goto(path);
  await page.waitForFunction(() => window.__viewer?.state?.ready === true, undefined, { timeout: 20_000 });
  expect(await page.evaluate(() => !!document.querySelector("canvas")?.getContext("webgl2")),
    "WebGL2 available").toBe(true);
  return errors;
}

test.describe("Melkor · SparkJS splat viewer", () => {
  test("serves splat assets", async ({ request }) => {
    for (const s of SCENES) {
      const ok = await served(request, s.file);
      if (s.optional && !ok) { test.info().annotations.push({ type: "skip-asset", description: s.file }); continue; }
      expect(ok, `${s.file} served (200)`).toBe(true);
    }
  });

  test("development server fails closed on malformed and private paths", async ({ request }) => {
    const malformed = await request.fetch("/%");
    expect(malformed.status()).toBe(400);
    expect((await request.fetch("/package.json")).status()).toBe(404);
    for (const path of [
      "/vendor/../package.json",
      "/vendor/..%2Fpackage.json",
      "/public/..%2Fsrc-tauri%2FCargo.toml",
      "/public/%2e%2e%2fsrc-tauri%2fCargo.toml",
    ]) {
      expect((await request.fetch(path)).status(), path).not.toBe(200);
    }
    expect((await request.post("/index.html")).status()).toBe(405);
  });

  test("controls stay within a narrow mobile viewport", async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await boot(page);
    const bounds = await page.evaluate(() => ["hud", "scenes", "bar"].map((id) => {
      const rect = document.getElementById(id).getBoundingClientRect();
      return { id, left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom };
    }));
    for (const rect of bounds) {
      expect(rect.left, `${rect.id} starts inside viewport`).toBeGreaterThanOrEqual(0);
      expect(rect.right, `${rect.id} ends inside viewport`).toBeLessThanOrEqual(375);
      expect(rect.top, `${rect.id} starts vertically inside viewport`).toBeGreaterThanOrEqual(0);
      expect(rect.bottom, `${rect.id} ends vertically inside viewport`).toBeLessThanOrEqual(667);
    }
  });

  test("opens a local splat offline and can reopen the same file", async ({ page, context }) => {
    const errors = await boot(page);
    await context.setOffline(true);
    try {
      await page.locator("#localFileInput").setInputFiles(LOCAL_SPLAT);
      const first = await page.evaluate(() => window.__viewer.waitRendered(30_000));
      expect(first.scene).toBe("local-file");
      expect(first.format).toBe("SPLAT");
      expect(first.source).toBe("local");
      expect(first.fileBytes).toBeGreaterThan(0);
      expect(first.splatCount).toBeGreaterThanOrEqual(4_000);
      expect(new URL(page.url()).searchParams.has("scene"), "local file names stay out of the URL")
        .toBe(false);
      expect(decodeURIComponent(page.url()), "the local filename stays out of the entire URL")
        .not.toContain("wave.splat");
      expect(await pixelStats(page)).toMatchObject({ nonBgFrac: expect.any(Number) });
      expect((await pixelStats(page)).nonBgFrac, "local scene renders while offline")
        .toBeGreaterThan(0.001);
      await expect(page.locator("#h-scene")).toContainText("wave.splat");
      await expect(page.locator("#h-source")).toContainText("local");
      await expect(page.locator('[data-scene="local-file"]')).toHaveAttribute("aria-pressed", "true");

      // The input is reset after selection, so choosing an unchanged file is
      // a real reload rather than a no-op.
      await page.locator("#localFileInput").setInputFiles(LOCAL_SPLAT);
      const reopened = await page.evaluate(() => window.__viewer.waitRendered(30_000));
      expect(reopened.splatCount).toBe(first.splatCount);
      expect(errors, "local import has no runtime errors").toEqual([]);
    } finally {
      await context.setOffline(false);
    }
  });

  test("drag and drop opens one local splat", async ({ page }) => {
    const errors = await boot(page);
    await page.evaluate(async () => {
      const response = await fetch("/public/splats/generated/wave.splat");
      const file = new File([await response.arrayBuffer()], "dropped-wave.splat", {
        type: "application/octet-stream",
      });
      const transfer = new DataTransfer();
      transfer.items.add(file);
      window.__localDropTransfer = transfer;
      window.dispatchEvent(new DragEvent("dragenter", { bubbles: true, dataTransfer: transfer }));
    });
    await expect(page.locator("#dropTarget")).toBeVisible();
    await page.evaluate(() => {
      window.dispatchEvent(new DragEvent("drop", {
        bubbles: true,
        dataTransfer: window.__localDropTransfer,
      }));
    });
    const stats = await page.evaluate(() => window.__viewer.waitRendered(30_000));
    expect(stats.scene).toBe("local-file");
    expect(stats.source).toBe("local");
    expect(stats.splatCount).toBeGreaterThanOrEqual(4_000);
    await expect(page.locator("#dropTarget")).toBeHidden();
    await expect(page.locator("#h-scene")).toContainText("dropped-wave.splat");
    expect(errors, "drop import has no runtime errors").toEqual([]);
  });

  test("unsupported local files preserve the scene and offer recovery", async ({ page }) => {
    await boot(page);
    await page.evaluate(async () => {
      await window.__viewer.load("wave-static");
      await window.__viewer.waitRendered(30_000);
    });
    const before = await page.evaluate(() => window.__viewer.getStats());
    await page.locator("#localFileInput").setInputFiles({
      name: "not-a-splat.txt",
      mimeType: "text/plain",
      buffer: Buffer.from("not a splat"),
    });
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alert");
    await expect(page.locator("#ov-title")).toContainText("Unsupported file type");
    await expect(page.getByRole("button", { name: "Choose another file" })).toBeVisible();
    expect((await page.evaluate(() => window.__viewer.getStats())).scene,
      "invalid input keeps the current scene").toBe(before.scene);

    // Cancelling the replacement chooser must not strand an actionless modal.
    const chooser = page.waitForEvent("filechooser");
    await page.getByRole("button", { name: "Choose another file" }).click();
    await (await chooser).setFiles([]);
    await expect(page.getByRole("button", { name: "Choose another file" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeVisible();

    await page.getByRole("button", { name: "Dismiss" }).click();
    await expect(page.locator("#overlay")).not.toHaveClass(/show/);
    expect((await page.evaluate(() => window.__viewer.getStats())).inputError).toBeNull();
  });

  test("damaged local replacement preserves the committed local scene", async ({ page }) => {
    await boot(page);
    await page.locator("#localFileInput").setInputFiles(LOCAL_SPLAT);
    const before = await page.evaluate(() => window.__viewer.waitRendered(30_000));
    expect(before.scene).toBe("local-file");
    await expect(page.locator('[data-scene="local-file"]')).toContainText("wave.splat");
    await expect(page.locator('[data-scene="local-file"]')).toHaveAttribute("aria-pressed", "true");

    await page.locator("#localFileInput").setInputFiles({
      name: "damaged.ply",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("not a ply file"),
    });
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alert");
    await expect(page.getByRole("button", { name: "Choose another file" })).toBeVisible();
    await expect(page.locator("#h-scene")).toContainText("wave.splat");
    await expect(page.locator('[data-scene="local-file"]')).toContainText("wave.splat");
    await expect(page.locator('[data-scene="local-file"]')).toHaveAttribute("aria-pressed", "true");
    const localCatalog = await page.evaluate(() =>
      window.__viewer.scenes.filter((scene) => scene.local));
    expect(localCatalog).toEqual([
      expect.objectContaining({ id: "local-file", label: "wave.splat" }),
    ]);

    await page.getByRole("button", { name: "Dismiss" }).click();
    expect((await page.evaluate(() => window.__viewer.getStats())).error).toBeNull();
    expect((await pixelStats(page)).nonBgFrac, "last good local scene stays visible")
      .toBeGreaterThan(0.001);
  });

  test("reduced-motion preference disables automatic orbit", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await boot(page);
    await page.evaluate(() => window.__viewer.waitRendered(120_000));
    const before = await page.evaluate(() => window.__viewer.getStats().camera);
    await page.waitForTimeout(700);
    const after = await page.evaluate(() => window.__viewer.getStats().camera);
    expect(Math.hypot(before[0] - after[0], before[1] - after[1], before[2] - after[2]))
      .toBeLessThan(0.001);
    expect(await page.locator("#orbitBtn").getAttribute("aria-pressed")).toBe("false");
  });

  for (const sc of SCENES) {
    const drivesCamera = FULL_RENDER || sc.id === "wave-static";
    const action = drivesCamera ? "renders" : "loads";
    const suffix = drivesCamera ? " and drives camera feeds" : " in software-renderer smoke mode";
    test(`${action} ${sc.id} (${sc.fmt})${suffix}`, async ({ page, request }) => {
      test.skip(sc.optional && !(await served(request, sc.file)), `${sc.file} not generated (optional)`);
      const renderTimeout = IS_CI
        ? (sc.id === "sutro" ? 210_000 : 150_000)
        : 120_000;
      if (IS_CI) test.setTimeout(renderTimeout + (drivesCamera ? 180_000 : 30_000));
      const errors = await boot(page, sc.id);

      const stats = await page.evaluate(async (timeout) =>
        await window.__viewer.waitRendered(timeout), renderTimeout);

      expect(stats.error, `no load error for ${sc.id}`).toBeFalsy();
      expect(stats.scene, `selected scene for ${sc.id}`).toBe(sc.id);
      expect(stats.format, `format for ${sc.id}`).toBe(sc.fmt);
      expect(stats.splatCount, `splat count for ${sc.id}`).toBeGreaterThanOrEqual(sc.min);

      // CI loads every format and waits for rendered frames, but reserves
      // compositor pixel sampling/sorting for the small project-owned
      // fixture. VIEWER_FULL_RENDER=1 restores the full hardware matrix.
      if (!drivesCamera) {
        expect(errors, `no runtime errors for ${sc.id}`).toEqual([]);
        return;
      }

      // Drive each named camera feed: distinct viewpoint, non-blank frame, screenshot.
      const cams = [], frameSignatures = [];
      for (const view of VIEWS) {
        await page.evaluate((v) => window.__viewer.setView(v), view);
        await page.waitForTimeout(IS_CI ? 1200 : 800); // let the splat sort settle for this viewpoint
        const px = await pixelStats(page);
        expect(px.nonBgFrac, `${sc.id}/${view} renders content`).toBeGreaterThan(0.012);
        expect(px.lumStd, `${sc.id}/${view} is not a uniform framebuffer`)
          .toBeGreaterThan(FULL_RENDER ? 4 : 0.5);
        frameSignatures.push(px.signature);
        cams.push((await page.evaluate(() => window.__viewer.getStats().camera)).join(","));
        await page.screenshot({ path: join(SHOT_DIR, `${sc.id}-${view}.png`) });
      }

      // Camera controls actually changed the viewpoint across feeds.
      expect(new Set(cams).size, `distinct camera positions for ${sc.id}`).toBeGreaterThan(1);
      expect(new Set(frameSignatures).size, `camera feeds change pixels for ${sc.id}`)
        .toBeGreaterThan(1);
      expect(errors, `no runtime errors for ${sc.id}`).toEqual([]);
    });
  }

  test("auto-orbit animates the camera over time", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    await page.evaluate(() => window.__viewer.setOrbit(true));
    const a = await page.evaluate(() => window.__viewer.getStats().camera);
    await page.waitForTimeout(1500);
    const b = await page.evaluate(() => window.__viewer.getStats().camera);
    const moved = Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    expect(moved, "camera moved while orbiting").toBeGreaterThan(0.05);
  });

  test("setAngles repositions the camera deterministically", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const left = await page.evaluate(() => { window.__viewer.setAngles(0, 12); return window.__viewer.getStats().camera; });
    const right = await page.evaluate(() => { window.__viewer.setAngles(180, 12); return window.__viewer.getStats().camera; });
    expect(Math.hypot(left[0] - right[0], left[2] - right[2]), "azimuth sweep moves camera").toBeGreaterThan(1);
  });

  // Progressive streaming: the first load (nothing on screen yet) adds the
  // mesh to the scene and renders splats as they stream in; a scene switch
  // (something already showing) keeps the old scene until the new one is
  // ready, so it is not progressive. Also verifies the mid-stream state:
  // the mesh is in the scene while still loading.
  test("first load streams progressively; scene switch does not", async ({ page }) => {
    await boot(page);
    // The boot auto-load had no prior mesh -> progressive.
    await page.waitForFunction(() => window.__viewer?.state?.rendered === true, undefined, { timeout: 60_000 });
    expect(await page.evaluate(() => window.__viewer.state.progressive),
      "boot load was progressive").toBe(true);

    // Kick off a switch to a different scene and sample state mid-load: the
    // previous scene must still be the one displayed (not progressive), and
    // the previous mesh stays in the scene until the new one is ready.
    const replacementScene = IS_CI ? "wave-static" : "valley";
    const midLoad = await page.evaluate(async (sceneId) => {
      const p = window.__viewer.load(sceneId);          // do not await
      // poll until loading flips true, then read the streaming decision
      for (let i = 0; i < 200 && !window.__viewer.state.loading; i++)
        await new Promise((r) => setTimeout(r, 5));
      const snap = { progressive: window.__viewer.state.progressive, loading: window.__viewer.state.loading };
      await p;
      return snap;
    }, replacementScene);
    expect(midLoad.loading, "load actually started").toBe(true);
    expect(midLoad.progressive, "scene switch keeps old scene (not progressive)").toBe(false);
  });

  // 4D temporal player: loads a per-frame splat sequence (manifest + N PLYs,
  // the shape a 4D-GS export produces) and plays it on a timeline. The demo
  // sequence is generated by `node make-4d-demo.js`; skip if not present.
  test("failed 4D entry preserves the active static scene", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    await boot(page);
    await page.evaluate(async () => {
      await window.__viewer.load("wave-static");
      await window.__viewer.waitRendered(30_000);
    });
    const before = await page.evaluate(() => window.__viewer.getStats());
    await page.route("**/4d/wave/manifest.json", async (route) => {
      if (route.request().method() === "GET") {
        await route.fulfill({ status: 500, contentType: "text/plain", body: "forced failure" });
      } else {
        await route.continue();
      }
    });
    await page.evaluate(() => window.__viewer.load("wave-4d").catch(() => {}));
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alert");
    await expect(page.getByRole("button", { name: "Retry" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeVisible();
    const retained = await page.evaluate(() => ({
      stats: window.__viewer.getStats(),
      sequence: window.__viewer.get4DState(),
    }));
    expect(retained.stats.scene).toBe(before.scene);
    expect(retained.sequence.count).toBe(0);
    expect((await pixelStats(page)).nonBgFrac, "active static scene remains visible")
      .toBeGreaterThan(0.001);

    await page.getByRole("button", { name: "Dismiss" }).click();
    await page.waitForFunction(() => window.__viewer.getStats().rendered === true);
    expect((await page.evaluate(() => window.__viewer.getStats())).error).toBeNull();
  });

  test("damaged local file cannot destroy the active 4D scene", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    await boot(page);
    await page.evaluate(async () => {
      await window.__viewer.load("wave-4d");
      await window.__viewer.waitRendered(120_000);
    });
    const before = await page.evaluate(() => ({
      scene: window.__viewer.getStats().scene,
      sequence: window.__viewer.get4DState(),
    }));
    expect(before.sequence.count).toBeGreaterThan(1);
    expect(before.sequence.buffered).toBeGreaterThan(0);

    await page.locator("#localFileInput").setInputFiles({
      name: "damaged.ply",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("not a ply file"),
    });
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alert");
    await expect(page.locator("#ov-title")).toContainText("Could not open damaged.ply");
    const retained = await page.evaluate(() => ({
      stats: window.__viewer.getStats(),
      sequence: window.__viewer.get4DState(),
    }));
    expect(retained.stats.scene).toBe(before.scene);
    expect(retained.sequence.count).toBe(before.sequence.count);
    expect(retained.sequence.buffered).toBeGreaterThan(0);
    expect((await pixelStats(page)).nonBgFrac, "active 4D scene remains visible")
      .toBeGreaterThan(0.001);

    await page.getByRole("button", { name: "Dismiss" }).click();
    await expect(page.locator("#overlay")).not.toHaveClass(/show/);
    expect((await page.evaluate(() => window.__viewer.getStats())).error).toBeNull();
  });

  test("4D temporal player sequences per-frame splats", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    const errors = await boot(page);
    await page.evaluate(async () => {
      await window.__viewer.load("wave-4d");
      await window.__viewer.waitRendered(120_000);
    });

    const st = await page.evaluate(() => window.__viewer.get4DState());
    expect(new URL(page.url()).searchParams.get("scene"), "selected scene is shareable")
      .toBe("wave-4d");
    expect(st.count, "sequence has multiple frames").toBeGreaterThan(1);
    expect(st.playing, "playback started on load").toBe(true);

    // Buffered-window streaming: only a window of frames is resident, so
    // memory is bounded below the full sequence length.
    expect(st.buffered, "only a window of frames is buffered (bounded memory)")
      .toBeLessThan(st.count);
    expect(st.buffered, "at least the active frame is buffered").toBeGreaterThan(0);

    // Frame advances over time while playing.
    const a = await page.evaluate(() => window.__viewer.get4DState().active);
    await page.waitForTimeout(700);
    const b = await page.evaluate(() => window.__viewer.get4DState().active);
    expect(a === b ? -1 : 1, "active frame advanced while playing").toBe(1);

    // The rendered image differs between two distinct frames (real motion).
    const shot = async (frame, previousSignature = null) => {
      await page.evaluate(async (f) => {
        await window.__viewer.seek4D(f);
        await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      }, frame);
      // SwiftShader can complete the asynchronous splat sort after the first
      // compositor frames. Poll the visible output for the requested new frame
      // instead of relying on a fixed machine-dependent delay.
      const deadline = Date.now() + (IS_CI ? 5_000 : 2_000);
      let stats;
      do {
        stats = await pixelStats(page);
        if (stats.lumStd > 0.5 &&
            (previousSignature === null || stats.signature !== previousSignature)) return stats;
        await page.waitForTimeout(100);
      } while (Date.now() < deadline);
      return stats;
    };
    const count = await page.evaluate(() => window.__viewer.get4DState().count);
    const s0 = await shot(0);
    const sMid = await shot(Math.floor(count / 2), s0.signature);
    expect(s0.lumStd, "first temporal frame is nonblank").toBeGreaterThan(0.5);
    expect(sMid.lumStd, "middle temporal frame is nonblank").toBeGreaterThan(0.5);
    expect(s0.signature, "frames render differently (temporal motion)").not.toBe(sMid.signature);

    // Seek to a far frame (outside the initial window) still works: the
    // player loads it on demand — proves streaming, not preloaded.
    const seeked = await page.evaluate(async () => {
      await window.__viewer.seek4D(3);
      const s = window.__viewer.get4DState();
      return { active: s.active, playing: s.playing, buffered: s.buffered, count: s.count };
    });
    expect(seeked.active, "seek sets the active frame").toBe(3);
    expect(seeked.playing, "seek pauses playback").toBe(false);
    expect(seeked.buffered, "buffer stays windowed after seeking").toBeLessThan(seeked.count);

    // Playback is a loop, not a one-shot. Seeking to the final frame must
    // prefetch frame 0 and cross the boundary without stalling.
    const wrapped = await page.evaluate(async () => {
      const count = window.__viewer.get4DState().count;
      await window.__viewer.seek4D(count - 1);
      window.__viewer.play4D();
      const deadline = performance.now() + 3000;
      while (performance.now() < deadline) {
        const state = window.__viewer.get4DState();
        if (state.active === 0) return state;
        await new Promise((resolve) => setTimeout(resolve, 25));
      }
      return window.__viewer.get4DState();
    });
    expect(wrapped.active, "playback wraps from the final frame to frame 0").toBe(0);
    expect(wrapped.playing, "playback remains active after wrapping").toBe(true);

    // Switch from the 4D sequence back to a normal scene: must clear the
    // frames cleanly (no double-dispose) and render the new scene.
    const staticScene = IS_CI ? "wave-static" : "snow-street";
    await page.evaluate(async (sceneId) => {
      await window.__viewer.load(sceneId);
      await window.__viewer.waitRendered(120_000);
    }, staticScene);
    const after = await page.evaluate(() => window.__viewer.get4DState().count);
    expect(after, "4D frames cleared on switch to normal scene").toBe(0);
    // Poll: a large scene loaded progressively may need a few extra frames
    // past waitRendered before its splats produce visible pixels.
    let frac = 0;
    for (let i = 0; i < 40 && frac <= 0.001; i++) {
      frac = (await pixelStats(page)).nonBgFrac;
      if (frac <= 0.001) await page.waitForTimeout(50);
    }
    expect(frac, "normal scene renders after leaving 4D").toBeGreaterThan(0.001);
    expect(errors, "no page errors").toEqual([]);
  });

  test("4D scrubbing is latest-seek-wins when frame loads resolve out of order", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    await page.route("**/4d/wave/time_00015.ply", async (route) => {
      await new Promise((resolve) => setTimeout(resolve, 750));
      await route.continue();
    });
    await boot(page);
    await page.evaluate(async () => {
      await window.__viewer.load("wave-4d");
      window.__viewer.pause4D();
      const stale = window.__viewer.seek4D(15);
      await new Promise((resolve) => setTimeout(resolve, 25));
      const latest = window.__viewer.seek4D(8);
      await Promise.all([stale, latest]);
    });
    await page.waitForTimeout(900);
    expect(await page.evaluate(() => window.__viewer.get4DState().active),
      "the delayed stale seek cannot overwrite the latest seek").toBe(8);
  });

  test("4D missing frames retry finitely and surface a recoverable error", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    let requests = 0;
    await page.route("**/4d/wave/time_00007.ply", async (route) => {
      requests++;
      await route.fulfill({ status: 404, contentType: "text/plain", body: "missing test frame" });
    });
    await boot(page);
    await page.evaluate(() => window.__viewer.load("wave-4d"));
    await page.waitForFunction(() => window.__viewer.get4DState().error !== null, undefined,
      { timeout: 8_000 });

    const failed = await page.evaluate(() => ({
      state: window.__viewer.get4DState(),
      retryVisible: !document.getElementById("ov-retry").hidden,
    }));
    expect(requests, "a missing frame has a strict retry cap").toBe(3);
    expect(failed.state.playing, "playback pauses rather than freezing silently").toBe(false);
    expect(failed.state.error, "the failure is visible to callers").toContain("after 3 attempts");
    expect(failed.retryVisible, "the viewer offers an explicit retry action").toBe(true);

    await page.getByRole("button", { name: "Retry" }).click();
    await page.waitForFunction(() => window.__viewer.get4DState().error !== null, undefined,
      { timeout: 8_000 });
    expect(requests, "a failed explicit retry runs one new bounded retry cycle").toBe(6);
    expect(await page.getByRole("button", { name: "Retry" }).isVisible(),
      "a repeated failure remains recoverable").toBe(true);
  });

  // The "4D format producer" (pack-4d.js) packs a per-frame sequence into
  // per-frame SPZ + a manifest; the temporal player streams it identically to
  // the PLY sequence. Skip if the SPZ pack isn't generated.
  test("4D player streams an SPZ-packed sequence (producer -> player)", async ({ page, request }) => {
    const has = (await request.fetch("/public/splats/4d/wave-spz/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!has, "SPZ 4D pack not generated (run node pack-4d.js ... --spz)");

    const errors = await boot(page);
    await page.evaluate(async () => {
      await window.__viewer.load("wave-4d-spz");
      await window.__viewer.waitRendered(120_000);
    });
    const st = await page.evaluate(() => window.__viewer.get4DState());
    expect(st.count, "SPZ sequence has multiple frames").toBeGreaterThan(1);
    expect(st.playing, "SPZ sequence plays on load").toBe(true);
    expect(st.buffered, "SPZ sequence is windowed").toBeLessThan(st.count);

    const a = await page.evaluate(() => window.__viewer.get4DState().active);
    await page.waitForTimeout(700);
    const b = await page.evaluate(() => window.__viewer.get4DState().active);
    expect(a === b ? -1 : 1, "SPZ sequence advances while playing").toBe(1);
    expect(errors, "no page errors").toEqual([]);
  });
});
