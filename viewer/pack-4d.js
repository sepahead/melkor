#!/usr/bin/env node
// Package a per-frame splat sequence (e.g. a 4D-GS `export_perframe_3DGS.py`
// output — time_00000.ply, time_00001.ply, ...) into a 4D scene the viewer's
// temporal player can stream: a manifest.json { fps, frames: [...] } and,
// optionally, per-frame SPZ compression via the melkor binary.
//
// 4D sequences are large (N frames x a full 3DGS cloud each), so compressing
// every frame PLY -> SPZ (~90% smaller, and the viewer plays SPZ natively)
// makes streaming practical. This is the "4D format producer" side of the
// pipeline: reconstruct (4D-GS) -> pack+compress (melkor) -> stream (viewer).
//
// Usage:
//   node pack-4d.js <frames_dir> [--spz] [--fps N] [--out <dir>] [--melkor <path>]
//
//   <frames_dir>  directory containing time_*.ply (or *.ply / *.spz)
//   --spz         compress each .ply to .spz with melkor (much smaller)
//   --fps N       manifest playback fps (default 12)
//   --out <dir>   output dir for frames + manifest.json
//                 (default: public/splats/4d/<basename of frames_dir>)
//   --melkor <p>  path to the melkor binary (default ../build/melkor)
import { readdirSync, mkdirSync, copyFileSync, writeFileSync, existsSync, statSync } from "node:fs";
import { join, basename, resolve } from "node:path";
import { spawnSync } from "node:child_process";

const args = process.argv.slice(2);
if (!args.length || args[0].startsWith("--")) {
  console.error("usage: node pack-4d.js <frames_dir> [--spz] [--fps N] [--out <dir>] [--melkor <path>]");
  process.exit(1);
}
const framesDir = args[0];
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const has = (name) => args.includes(name);
const useSpz = has("--spz");
const fps = Number(opt("--fps", "12"));
const outDir = resolve(opt("--out", join("public/splats/4d", basename(framesDir.replace(/\/+$/, "")))));
const melkor = resolve(opt("--melkor", "../build/melkor"));

if (!existsSync(framesDir) || !statSync(framesDir).isDirectory()) {
  console.error(`error: frames_dir not found: ${framesDir}`);
  process.exit(1);
}

// Collect frame files. Prefer PLY (compressible); fall back to SPZ. Sort by the
// trailing integer so time_00010 follows time_00009, not lexically.
const frameNum = (f) => { const m = f.match(/(\d+)(?=\.[^.]+$)/); return m ? Number(m[1]) : 0; };
const all = readdirSync(framesDir);
let src = all.filter((f) => /\.ply$/i.test(f));
let srcExt = "ply";
if (!src.length) { src = all.filter((f) => /\.spz$/i.test(f)); srcExt = "spz"; }
if (!src.length) { console.error(`error: no .ply or .spz frames in ${framesDir}`); process.exit(1); }
src.sort((a, b) => frameNum(a) - frameNum(b) || a.localeCompare(b));

if (useSpz && srcExt !== "ply") { console.error("--spz needs .ply source frames"); process.exit(1); }
if (useSpz && !existsSync(melkor)) {
  console.error(`error: --spz needs the melkor binary; not found at ${melkor} (build it, or pass --melkor)`);
  process.exit(1);
}

mkdirSync(outDir, { recursive: true });
const frames = [];
let inBytes = 0, outBytes = 0;
for (const f of src) {
  const inPath = join(framesDir, f);
  inBytes += statSync(inPath).size;
  if (useSpz) {
    const outName = f.replace(/\.ply$/i, ".spz");
    const outPath = join(outDir, outName);
    const r = spawnSync(melkor, [inPath, outPath], { stdio: ["ignore", "ignore", "inherit"] });
    if (r.status !== 0 || !existsSync(outPath)) { console.error(`error: melkor failed on ${f}`); process.exit(1); }
    outBytes += statSync(outPath).size;
    frames.push(outName);
  } else {
    const outPath = join(outDir, f);
    if (resolve(inPath) !== resolve(outPath)) copyFileSync(inPath, outPath);
    outBytes += statSync(outPath).size;
    frames.push(f);
  }
}

writeFileSync(join(outDir, "manifest.json"), JSON.stringify({ fps, frames }, null, 2) + "\n");
const mb = (n) => (n / 1e6).toFixed(2);
console.log(`packed ${frames.length} frames -> ${outDir}/manifest.json`);
if (useSpz) console.log(`compressed ${mb(inBytes)} MB PLY -> ${mb(outBytes)} MB SPZ (${(100 * (1 - outBytes / inBytes)).toFixed(0)}% smaller)`);
console.log(`add a viewer scene: { id, label, manifest: "4d/${basename(outDir)}/manifest.json", fmt: "4D-SPZ", temporal: true, optional: true }`);
