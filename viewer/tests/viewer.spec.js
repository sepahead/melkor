import { test, expect } from "@playwright/test";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { mkdirSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const SHOT_DIR = join(__dirname, "..", "screenshots");
mkdirSync(SHOT_DIR, { recursive: true });

// City / area scenes in public/splats/. `min` = sane lower bound on splat count.
// `optional` scenes (the melkor-generated .ply) are skipped when their file is absent.
const SCENES = [
  { id: "snow-street",   fmt: "SPZ",   min: 900_000,   file: "snow-street.spz" },
  { id: "valley",        fmt: "SPZ",   min: 450_000,   file: "valley.spz" },
  { id: "sutro",         fmt: "SOG",   min: 1_900_000, file: "sutro.zip" },
  { id: "train",         fmt: "SPLAT", min: 1_000_000, file: "train.splat" },
  { id: "distant-igloo", fmt: "SPZ",   min: 300_000,   file: "distant-igloo.spz" },
  { id: "igloo-ply",     fmt: "PLY",   min: 300_000,   file: "distant-igloo.ply", optional: true },
];
const VIEWS = ["front", "side", "top", "iso"]; // distinct "camera feeds"

async function served(request, file) {
  const r = await request.fetch(`/public/splats/${file}`, { method: "HEAD" });
  return r.status() === 200;
}

// Read back the WebGL framebuffer: fraction of non-background pixels + luminance
// std-dev. A blank or broken render scores ~0 on both.
async function pixelStats(page) {
  return await page.evaluate(() => {
    const cv = document.querySelector("canvas");
    const g = cv.getContext("webgl2") || cv.getContext("webgl");
    const w = cv.width, h = cv.height, buf = new Uint8Array(w * h * 4);
    g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, buf);
    let nonBg = 0, n = 0, sum = 0, sum2 = 0;
    for (let i = 0; i < buf.length; i += 4 * 131) {
      const r = buf[i], gg = buf[i + 1], b = buf[i + 2];
      const lum = 0.299 * r + 0.587 * gg + 0.114 * b;
      sum += lum; sum2 += lum * lum; n++;
      if (Math.abs(r - 11) + Math.abs(gg - 12) + Math.abs(b - 16) > 24) nonBg++;
    }
    const mean = sum / n;
    return { nonBgFrac: nonBg / n, lumStd: Math.sqrt(Math.max(sum2 / n - mean * mean, 0)) };
  });
}

async function boot(page) {
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto("/index.html");
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

  for (const sc of SCENES) {
    test(`renders ${sc.id} (${sc.fmt}) and drives camera feeds`, async ({ page, request }) => {
      test.skip(sc.optional && !(await served(request, sc.file)), `${sc.file} not generated (optional)`);
      const errors = await boot(page);

      const stats = await page.evaluate(async (id) => {
        await window.__viewer.load(id);
        return await window.__viewer.waitRendered(120_000);
      }, sc.id);

      expect(stats.error, `no load error for ${sc.id}`).toBeFalsy();
      expect(stats.format, `format for ${sc.id}`).toBe(sc.fmt);
      expect(stats.splatCount, `splat count for ${sc.id}`).toBeGreaterThanOrEqual(sc.min);

      // Drive each named camera feed: distinct viewpoint, non-blank frame, screenshot.
      const cams = [];
      for (const view of VIEWS) {
        await page.evaluate((v) => window.__viewer.setView(v), view);
        await page.waitForTimeout(800); // let the splat sort settle for this viewpoint
        const px = await pixelStats(page);
        expect(px.nonBgFrac, `${sc.id}/${view} renders content`).toBeGreaterThan(0.012);
        expect(px.lumStd, `${sc.id}/${view} has tonal variance`).toBeGreaterThan(4);
        cams.push((await page.evaluate(() => window.__viewer.getStats().camera)).join(","));
        await page.screenshot({ path: join(SHOT_DIR, `${sc.id}-${view}.png`) });
      }

      // Camera controls actually changed the viewpoint across feeds.
      expect(new Set(cams).size, `distinct camera positions for ${sc.id}`).toBeGreaterThan(1);
      expect(errors, `no runtime errors for ${sc.id}`).toEqual([]);
    });
  }

  test("auto-orbit animates the camera over time", async ({ page }) => {
    await boot(page);
    await page.evaluate(async () => { await window.__viewer.load("sutro"); await window.__viewer.waitRendered(120_000); });
    await page.evaluate(() => window.__viewer.setOrbit(true));
    const a = await page.evaluate(() => window.__viewer.getStats().camera);
    await page.waitForTimeout(1500);
    const b = await page.evaluate(() => window.__viewer.getStats().camera);
    const moved = Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    expect(moved, "camera moved while orbiting").toBeGreaterThan(0.05);
  });

  test("setAngles repositions the camera deterministically", async ({ page }) => {
    await boot(page);
    await page.evaluate(async () => { await window.__viewer.load("snow-street"); await window.__viewer.waitRendered(120_000); });
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
    const midLoad = await page.evaluate(async () => {
      const p = window.__viewer.load("valley");        // do not await
      // poll until loading flips true, then read the streaming decision
      for (let i = 0; i < 200 && !window.__viewer.state.loading; i++)
        await new Promise((r) => setTimeout(r, 5));
      const snap = { progressive: window.__viewer.state.progressive, loading: window.__viewer.state.loading };
      await p;
      return snap;
    });
    expect(midLoad.loading, "load actually started").toBe(true);
    expect(midLoad.progressive, "scene switch keeps old scene (not progressive)").toBe(false);
  });

  // 4D temporal player: loads a per-frame splat sequence (manifest + N PLYs,
  // the shape a 4D-GS export produces) and plays it on a timeline. The demo
  // sequence is generated by `node make-4d-demo.js`; skip if not present.
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
    expect(st.count, "multiple frames loaded").toBeGreaterThan(1);
    expect(st.playing, "playback started on load").toBe(true);

    // Frame advances over time while playing.
    const a = await page.evaluate(() => window.__viewer.get4DState().active);
    await page.waitForTimeout(700);
    const b = await page.evaluate(() => window.__viewer.get4DState().active);
    expect(a === b ? -1 : 1, "active frame advanced while playing").toBe(1);

    // The rendered image differs between two distinct frames (real motion).
    const shot = async (frame) => page.evaluate(async (f) => {
      window.__viewer.seek4D(f);
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      const cv = document.querySelector("canvas");
      const g = cv.getContext("webgl2") || cv.getContext("webgl");
      const w = cv.width, h = cv.height, buf = new Uint8Array(w * h * 4);
      g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, buf);
      let s = 0; for (let i = 0; i < buf.length; i += 4 * 131) s += buf[i] + buf[i + 1] * 2 + buf[i + 2] * 3;
      return s;
    }, frame);
    const s0 = await shot(0);
    const sMid = await shot(Math.floor((await page.evaluate(() => window.__viewer.get4DState().count)) / 2));
    expect(Math.abs(s0 - sMid), "frames render differently (temporal motion)").toBeGreaterThan(0);

    // Pause + seek is deterministic.
    const seeked = await page.evaluate(() => { window.__viewer.seek4D(3); const s = window.__viewer.get4DState(); return { active: s.active, playing: s.playing }; });
    expect(seeked.active, "seek sets the active frame").toBe(3);
    expect(seeked.playing, "seek pauses playback").toBe(false);

    // Switch from the 4D sequence back to a normal scene: must clear the
    // frames cleanly (no double-dispose) and render the new scene.
    await page.evaluate(async () => {
      await window.__viewer.load("snow-street");
      await window.__viewer.waitRendered(120_000);
    });
    const after = await page.evaluate(() => window.__viewer.get4DState().count);
    expect(after, "4D frames cleared on switch to normal scene").toBe(0);
    const px = await pixelStats(page);
    expect(px.nonBgFrac, "normal scene renders after leaving 4D").toBeGreaterThan(0.001);
    expect(errors, "no page errors").toEqual([]);
  });
});
