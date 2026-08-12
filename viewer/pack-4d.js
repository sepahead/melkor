#!/usr/bin/env node
// Package a per-frame splat sequence (e.g. a 4D-GS `export_perframe_3DGS.py`
// output — time_00000.ply, time_00001.ply, ...) into a 4D scene the viewer's
// temporal player can stream: a manifest.json { fps, frames: [...] } and,
// optionally, per-frame SPZ compression via the melkor binary.
//
// Usage:
//   node pack-4d.js <frames_dir> [--spz] [--fps N] [--out <dir>]
//                   [--melkor <path>] [Melkor conversion options]
//
//   <frames_dir>  directory containing time_*.ply (or *.ply / *.spz)
//   --spz         compress each .ply to .spz with melkor
//   --fps N       manifest playback fps (default 12)
//   --out <dir>   output dir for frames + manifest.json
//                 (default: public/splats/4d/<basename of frames_dir>)
//   --melkor <p>  path to the melkor binary
//   Selected input, output, policy, and loss options pass through to Melkor.
import {
  copyFileSync,
  existsSync,
  lstatSync,
  mkdtempSync,
  mkdirSync,
  readdirSync,
  rmSync,
  realpathSync,
  writeFileSync,
} from "node:fs";
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
  sep,
} from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { validateLossReport } from "./loss-report-validator.js";

const args = process.argv.slice(2);
const viewerDir = dirname(fileURLToPath(import.meta.url));
const publishTool = join(viewerDir, "..", "tools", "atomic_publish.py");
const MAX_FRAME_BYTES = 256 * 1024 * 1024;
const fail = (message) => {
  console.error(`error: ${message}`);
  process.exit(2);
};
const usage = () => {
  console.log(
    "usage: node pack-4d.js <frames_dir> [--spz] [--fps N] " +
    "[--out <dir>] [--melkor <path>] [Melkor conversion options]",
  );
};

let framesDir = "";
let useSpz = false;
let fpsText = "12";
let outOption = "";
let melkorOption = "";
const melkorArgs = [];
const melkorValueOptions = new Set([
  "--input-profile",
  "--source-frame",
  "--source-unit-to-meter",
  "--source-color-space",
  "--output-antialiased",
  "--max-sh-degree",
  "--limits-profile",
  "--allow-loss",
]);

for (let index = 0; index < args.length; index += 1) {
  const arg = args[index];
  if (arg === "--spz") {
    useSpz = true;
  } else if (arg === "--help" || arg === "-h") {
    usage();
    process.exit(0);
  } else if (["--fps", "--out", "--melkor", ...melkorValueOptions].includes(arg)) {
    const value = args[index + 1];
    if (!value || value.startsWith("--")) fail(`missing value for ${arg}`);
    if (arg === "--fps") fpsText = value;
    if (arg === "--out") outOption = value;
    if (arg === "--melkor") melkorOption = value;
    if (melkorValueOptions.has(arg)) melkorArgs.push(arg, value);
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
const requestedSourceDir = resolve(framesDir);
const requestedOutDir = outOption
  ? resolve(outOption)
  : join(viewerDir, "public", "splats", "4d", basename(requestedSourceDir));
const isWithin = (parent, child) => {
  const path = relative(parent, child);
  return path === "" || (!path.startsWith(`..${sep}`) && path !== ".." && !isAbsolute(path));
};
if (dirname(requestedOutDir) === requestedOutDir) {
  fail("output directory cannot be a filesystem root");
}
if (isWithin(requestedOutDir, requestedSourceDir) ||
    isWithin(requestedSourceDir, requestedOutDir)) {
  fail("output directory must not overlap frames_dir");
}
if (!existsSync(requestedSourceDir) || !lstatSync(requestedSourceDir).isDirectory()) {
  fail(`frames_dir not found: ${framesDir}`);
}
function canonicalFuturePath(path) {
  const suffix = [basename(path)];
  let ancestor = dirname(path);
  while (!existsSync(ancestor)) {
    const parent = dirname(ancestor);
    if (parent === ancestor) fail(`cannot resolve the output parent: ${path}`);
    suffix.unshift(basename(ancestor));
    ancestor = parent;
  }
  const canonicalAncestor = realpathSync(ancestor);
  if (!lstatSync(canonicalAncestor).isDirectory()) {
    fail(`output parent is not a directory: ${ancestor}`);
  }
  return resolve(canonicalAncestor, ...suffix);
}
const sourceDir = realpathSync(requestedSourceDir);
const outDir = canonicalFuturePath(requestedOutDir);
if (isWithin(outDir, sourceDir) || isWithin(sourceDir, outDir)) {
  fail("output directory must not overlap frames_dir after path resolution");
}
for (const protectedPath of [resolve(), viewerDir, dirname(viewerDir)]
  .map((path) => realpathSync(path))) {
  if (isWithin(outDir, protectedPath)) {
    fail(`output directory cannot contain a protected project path: ${outDir}`);
  }
}
if (existsSync(outDir)) {
  fail(`output directory already exists: ${outDir}`);
}
const defaultMelkor = [join(viewerDir, "..", "build", "dev", "melkor"),
  join(viewerDir, "..", "build", "melkor")]
  .map((candidate) => resolve(candidate))
  .find((candidate) => existsSync(candidate));
const melkor = resolve(
  melkorOption || defaultMelkor || join(viewerDir, "..", "build", "dev", "melkor"),
);

// Collect frame files. Prefer PLY (compressible); fall back to SPZ. Sort by the
// trailing integer so time_00010 follows time_00009, not lexically.
const frameNum = (f) => { const m = f.match(/(\d+)(?=\.[^.]+$)/); return m ? Number(m[1]) : 0; };
const all = readdirSync(sourceDir);
let src = all.filter((f) => /\.ply$/i.test(f));
let srcExt = "ply";
if (!src.length) { src = all.filter((f) => /\.spz$/i.test(f)); srcExt = "spz"; }
if (!src.length) fail(`no .ply or .spz frames in ${framesDir}`);
src.sort((a, b) => frameNum(a) - frameNum(b) || a.localeCompare(b));
if (src.length > 10_000) fail("a viewer sequence can contain at most 10,000 frames");

const portableNames = new Set();
for (const name of src) {
  if (!/^[A-Za-z0-9._-]+$/.test(name)) fail(`unsafe frame name: ${name}`);
  const source = lstatSync(join(sourceDir, name));
  if (!source.isFile()) fail(`frame is not a regular file: ${name}`);
  if (source.size === 0) fail(`empty frame file: ${name}`);
  if (source.size > MAX_FRAME_BYTES) fail(`frame exceeds 256 MiB: ${name}`);
  const portableName = name.toLowerCase();
  if (portableNames.has(portableName)) fail(`frame name is not portable: ${name}`);
  portableNames.add(portableName);
}

if (useSpz && srcExt !== "ply") fail("--spz needs .ply source frames");
if (!useSpz && melkorArgs.length > 0) fail("Melkor conversion options need --spz");
if (useSpz && !existsSync(melkor)) {
  fail(`--spz needs the melkor binary; not found at ${melkor}`);
}

function publishDirectory(staged, target) {
  const result = spawnSync("python3", [publishTool, staged, target], {
    encoding: "utf8",
    stdio: ["ignore", "ignore", "pipe"],
  });
  if (result.status !== 0) {
    throw new Error(`could not publish the sequence: ${result.stderr.trim()}`);
  }
}

mkdirSync(dirname(outDir), { recursive: true });
const stageDir = mkdtempSync(join(dirname(outDir), ".melkor-pack-stage-"));
const frames = [];
const lossReports = [];
let inBytes = 0, outBytes = 0;
try {
  for (const f of src) {
    const inPath = join(sourceDir, f);
    inBytes += lstatSync(inPath).size;
    if (useSpz) {
      const outName = f.replace(/\.ply$/i, ".spz");
      const outPath = join(stageDir, outName);
      const convertArgs = ["convert", inPath, outPath, ...melkorArgs];
      const result = spawnSync(melkor, convertArgs, {
        encoding: "utf8",
        maxBuffer: 16 * 1024 * 1024,
        stdio: ["ignore", "pipe", "inherit"],
      });
      if (result.status !== 0 || !existsSync(outPath) || lstatSync(outPath).size === 0) {
        throw new Error(`melkor failed on ${f}${result.error ? `: ${result.error.message}` : ""}`);
      }
      if (!lstatSync(outPath).isFile() || lstatSync(outPath).size > MAX_FRAME_BYTES) {
        throw new Error(`melkor produced an invalid or oversized frame for ${f}`);
      }
      let report;
      try {
        report = JSON.parse(result.stdout);
      } catch {
        throw new Error(`melkor returned an invalid loss report for ${f}`);
      }
      try {
        validateLossReport(report, { inputFormat: "ply", outputFormat: "spz" });
      } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new Error(`melkor returned an invalid loss report for ${f}: ${detail}`);
      }
      lossReports.push(report);
      outBytes += lstatSync(outPath).size;
      frames.push(outName);
    } else {
      const outPath = join(stageDir, f);
      copyFileSync(inPath, outPath);
      outBytes += lstatSync(outPath).size;
      frames.push(f);
    }
  }

  const manifest = { fps, frames };
  if (useSpz) {
    manifest.lossReports = "loss-reports.ndjson";
    writeFileSync(
      join(stageDir, manifest.lossReports),
      lossReports.map((report) => JSON.stringify(report)).join("\n") + "\n",
    );
  }
  const manifestText = JSON.stringify(manifest, null, 2) + "\n";
  if (Buffer.byteLength(manifestText) > 1024 * 1024) {
    throw new Error("the viewer manifest exceeds 1 MiB");
  }
  writeFileSync(join(stageDir, "manifest.json"), manifestText);
  publishDirectory(stageDir, outDir);
} catch (error) {
  if (existsSync(stageDir)) rmSync(stageDir, { recursive: true, force: true });
  fail(error instanceof Error ? error.message : String(error));
}

const mb = (n) => (n / 1e6).toFixed(2);
console.log(`packed ${frames.length} frames -> ${outDir}/manifest.json`);
if (useSpz) {
  const percent = Math.abs(100 * (1 - outBytes / inBytes)).toFixed(0);
  const relation = outBytes <= inBytes ? "smaller" : "larger";
  console.log(`compressed ${mb(inBytes)} MB PLY -> ${mb(outBytes)} MB SPZ (${percent}% ${relation})`);
}
const formatLabel = useSpz ? "4D-SPZ" : `4D-${srcExt.toUpperCase()}`;
console.log(
  `add a viewer scene: { id, label, manifest: "4d/${basename(outDir)}/manifest.json", ` +
  `fmt: "${formatLabel}", temporal: true, optional: true }`,
);
