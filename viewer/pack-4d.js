#!/usr/bin/env node
// Package a per-frame splat sequence (e.g. a 4D-GS `export_perframe_3DGS.py`
// output — time_00000.ply, time_00001.ply, ...) into a 4D scene the viewer's
// temporal player can stream: a manifest.json { fps, frames: [...] } and,
// optionally, per-frame SPZ compression via the melkor binary.
//
// Usage:
//   node pack-4d.js <frames_dir> [--spz] [--fps N] [--out <dir>]
//                   [--melkor <path>] [--force]
//
//   <frames_dir>  directory containing time_*.ply (or *.ply / *.spz)
//   --spz         compress each .ply to .spz with melkor
//   --fps N       manifest playback fps (default 12)
//   --out <dir>   output dir for frames + manifest.json
//                 (default: public/splats/4d/<basename of frames_dir>)
//   --melkor <p>  path to the melkor binary
//   --force       permit use of an existing output directory
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  renameSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { join, basename, resolve } from "node:path";
import { spawnSync } from "node:child_process";

const args = process.argv.slice(2);
const fail = (message) => {
  console.error(`error: ${message}`);
  process.exit(2);
};
const usage = () => {
  console.log(
    "usage: node pack-4d.js <frames_dir> [--spz] [--fps N] " +
    "[--out <dir>] [--melkor <path>] [--force]",
  );
};

let framesDir = "";
let useSpz = false;
let force = false;
let fpsText = "12";
let outOption = "";
let melkorOption = "";

for (let index = 0; index < args.length; index += 1) {
  const arg = args[index];
  if (arg === "--spz") {
    useSpz = true;
  } else if (arg === "--force") {
    force = true;
  } else if (arg === "--help" || arg === "-h") {
    usage();
    process.exit(0);
  } else if (["--fps", "--out", "--melkor"].includes(arg)) {
    const value = args[index + 1];
    if (!value || value.startsWith("--")) fail(`missing value for ${arg}`);
    if (arg === "--fps") fpsText = value;
    if (arg === "--out") outOption = value;
    if (arg === "--melkor") melkorOption = value;
    index += 1;
  } else if (arg.startsWith("--")) {
    fail(`unknown option: ${arg}`);
  } else if (!framesDir) {
    framesDir = arg;
  } else {
    fail(`unexpected argument: ${arg}`);
  }
}

if (!framesDir) {
  usage();
  process.exit(2);
}

const fps = Number(fpsText);
if (!Number.isFinite(fps) || fps <= 0 || fps > 240) {
  fail("--fps must be greater than zero and no more than 240");
}
const outDir = resolve(
  outOption || join("public/splats/4d", basename(framesDir.replace(/\/+$/, ""))),
);
const defaultMelkor = ["../build/dev/melkor", "../build/melkor"]
  .map((candidate) => resolve(candidate))
  .find((candidate) => existsSync(candidate));
const melkor = resolve(melkorOption || defaultMelkor || "../build/dev/melkor");

if (!existsSync(framesDir) || !statSync(framesDir).isDirectory()) {
  fail(`frames_dir not found: ${framesDir}`);
}

// Collect frame files. Prefer PLY (compressible); fall back to SPZ. Sort by the
// trailing integer so time_00010 follows time_00009, not lexically.
const frameNum = (f) => { const m = f.match(/(\d+)(?=\.[^.]+$)/); return m ? Number(m[1]) : 0; };
const all = readdirSync(framesDir);
let src = all.filter((f) => /\.ply$/i.test(f));
let srcExt = "ply";
if (!src.length) { src = all.filter((f) => /\.spz$/i.test(f)); srcExt = "spz"; }
if (!src.length) fail(`no .ply or .spz frames in ${framesDir}`);
src.sort((a, b) => frameNum(a) - frameNum(b) || a.localeCompare(b));

for (const name of src) {
  if (!/^[A-Za-z0-9._-]+$/.test(name)) fail(`unsafe frame name: ${name}`);
  if (statSync(join(framesDir, name)).size === 0) fail(`empty frame file: ${name}`);
}

if (useSpz && srcExt !== "ply") fail("--spz needs .ply source frames");
if (useSpz && !existsSync(melkor)) {
  fail(`--spz needs the melkor binary; not found at ${melkor}`);
}

if (existsSync(outDir) && readdirSync(outDir).length > 0 && !force) {
  fail(`output directory is not empty; use --force to permit its use: ${outDir}`);
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
    if (r.status !== 0 || !existsSync(outPath) || statSync(outPath).size === 0) {
      fail(`melkor failed on ${f}`);
    }
    outBytes += statSync(outPath).size;
    frames.push(outName);
  } else {
    const outPath = join(outDir, f);
    if (resolve(inPath) !== resolve(outPath)) copyFileSync(inPath, outPath);
    outBytes += statSync(outPath).size;
    frames.push(f);
  }
}

const manifestPath = join(outDir, "manifest.json");
const manifestTemp = `${manifestPath}.part-${process.pid}`;
writeFileSync(manifestTemp, JSON.stringify({ fps, frames }, null, 2) + "\n");
renameSync(manifestTemp, manifestPath);
const mb = (n) => (n / 1e6).toFixed(2);
console.log(`packed ${frames.length} frames -> ${outDir}/manifest.json`);
if (useSpz) {
  const percent = (100 * (1 - outBytes / inBytes)).toFixed(0);
  console.log(`compressed ${mb(inBytes)} MB PLY -> ${mb(outBytes)} MB SPZ (${percent}% change)`);
}
const formatLabel = useSpz ? "4D-SPZ" : `4D-${srcExt.toUpperCase()}`;
console.log(
  `add a viewer scene: { id, label, manifest: "4d/${basename(outDir)}/manifest.json", ` +
  `fmt: "${formatLabel}", temporal: true, optional: true }`,
);
