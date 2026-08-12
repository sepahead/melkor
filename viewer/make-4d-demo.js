#!/usr/bin/env node
// Generate a small synthetic 4D (temporal) splat sequence for the viewer's
// 4D player, in the same shape a real 4D-GS export produces: one standard
// 3DGS-layout binary PLY per timestamp (time_00000.ply, time_00001.ply, ...)
// plus a manifest.json listing them and the playback fps.
//
// The content is a traveling sine wave on a plane, so consecutive frames
// differ visibly (the player and its test can detect motion). Real 4D
// content: run 4D-GS `export_perframe_3DGS.py`, drop its time_*.ply into a
// dir, and write a manifest.json — no format translation (see
// docs/STREAMING.md).
//
// Usage: node make-4d-demo.js [outDir] [frames] [side]
import {
  existsSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const viewerDir = dirname(fileURLToPath(import.meta.url));
const publishTool = join(viewerDir, "..", "tools", "atomic_publish.py");
const requestedOutDir = resolve(
  process.argv[2] || join(viewerDir, "public", "splats", "4d", "wave"),
);
const FRAMES = Number(process.argv[3] || 24);
const SIDE = Number(process.argv[4] || 40);
if (!Number.isInteger(FRAMES) || FRAMES < 1 || FRAMES > 240) {
  throw new Error("frames must be an integer in [1, 240]");
}
if (!Number.isInteger(SIDE) || SIDE < 2 || SIDE > 512) {
  throw new Error("side must be an integer in [2, 512]");
}
if (FRAMES * SIDE * SIDE > 10_000_000) {
  throw new Error("the sequence must contain no more than 10,000,000 splats");
}
if (dirname(requestedOutDir) === requestedOutDir) {
  throw new Error("the output directory cannot be a filesystem root");
}
const isWithin = (parent, child) => {
  const path = relative(parent, child);
  return path === "" || (!path.startsWith(`..${sep}`) && path !== ".." && !isAbsolute(path));
};
function canonicalFuturePath(path) {
  const suffix = [basename(path)];
  let ancestor = dirname(path);
  while (!existsSync(ancestor)) {
    const parent = dirname(ancestor);
    if (parent === ancestor) throw new Error(`cannot resolve the output parent: ${path}`);
    suffix.unshift(basename(ancestor));
    ancestor = parent;
  }
  const canonicalAncestor = realpathSync(ancestor);
  if (!lstatSync(canonicalAncestor).isDirectory()) {
    throw new Error(`the output parent is not a directory: ${ancestor}`);
  }
  return resolve(canonicalAncestor, ...suffix);
}
const outDir = canonicalFuturePath(requestedOutDir);
for (const protectedPath of [resolve(), viewerDir, dirname(viewerDir)]
  .map((path) => realpathSync(path))) {
  if (isWithin(outDir, protectedPath)) {
    throw new Error(`the output directory cannot contain a protected project path: ${outDir}`);
  }
}
if (existsSync(outDir)) {
  if (!lstatSync(outDir).isDirectory()) throw new Error("the output path must be a directory");
  for (const name of readdirSync(outDir)) {
    if (name !== "manifest.json" && !/^time_[0-9]{5}\.ply$/.test(name)) {
      throw new Error(`the output directory contains an unrelated entry: ${name}`);
    }
    if (!lstatSync(join(outDir, name)).isFile()) {
      throw new Error(`the output directory contains a non-file entry: ${name}`);
    }
  }
}
mkdirSync(dirname(outDir), { recursive: true });
const stageDir = mkdtempSync(join(dirname(outDir), ".melkor-4d-stage-"));

// 3DGS canonical PLY property order (matches melkor's PlyWriter / INRIA).
const PROPS = [
  "x", "y", "z", "nx", "ny", "nz",
  "f_dc_0", "f_dc_1", "f_dc_2",
  "opacity", "scale_0", "scale_1", "scale_2",
  "rot_0", "rot_1", "rot_2", "rot_3",
];
const SH_C0 = 0.28209479177387814;
const rgb2sh = (c) => (c - 0.5) / SH_C0;
const logit = (p) => Math.log(p / (1 - p));

function frame(t) {
  const n = SIDE * SIDE;
  const header =
    "ply\nformat binary_little_endian 1.0\n" +
    "comment melkor_profile graphdeco-3dgs-v1\n" +
    "comment melkor_coordinate_system gltf-luf\n" +
    "comment melkor_length_unit meter\n" +
    "comment melkor_color_space lin_rec709_display\n" +
    "comment melkor_quaternion_order wxyz\n" +
    "comment melkor_scale_domain log\n" +
    "comment melkor_opacity_domain logit\n" +
    "comment melkor_sh_basis real_condon_shortley\n" +
    "comment melkor_sh_degree 0\n" +
    "comment melkor_antialiased 0\n" +
    `element vertex ${n}\n` +
    PROPS.map((p) => `property float ${p}`).join("\n") + "\n" +
    "end_header\n";
  const headerBuf = Buffer.from(header, "ascii");
  const body = Buffer.alloc(n * PROPS.length * 4);
  let o = 0;
  const put = (v) => { body.writeFloatLE(v, o); o += 4; };
  const phase = (t / FRAMES) * Math.PI * 2;
  const sLog = Math.log(0.02);
  const zLog = Math.log(0.006);
  for (let gy = 0; gy < SIDE; gy++) {
    for (let gx = 0; gx < SIDE; gx++) {
      const x = (gx / (SIDE - 1) - 0.5) * 2;
      const y = (gy / (SIDE - 1) - 0.5) * 2;
      const z = 0.35 * Math.sin(3.0 * x + phase) * Math.cos(2.0 * y + phase);
      put(x); put(y); put(z);          // position
      put(0); put(0); put(0);          // unused Graphdeco normal
      // color travels with the wave so motion is visible
      const c = 0.5 + 0.5 * Math.sin(3.0 * x + phase);
      put(rgb2sh(c)); put(rgb2sh(0.3)); put(rgb2sh(1 - c)); // f_dc
      put(logit(0.9));                 // opacity (logit)
      put(sLog); put(sLog); put(zLog); // scale (log)
      put(1); put(0); put(0); put(0);  // rotation (identity quaternion)
    }
  }
  return Buffer.concat([headerBuf, body]);
}

const frames = [];
let action = "wrote";
try {
  for (let i = 0; i < FRAMES; i++) {
    const name = `time_${String(i).padStart(5, "0")}.ply`;
    writeFileSync(join(stageDir, name), frame(i));
    frames.push(name);
  }
  writeFileSync(
    join(stageDir, "manifest.json"),
    JSON.stringify({ fps: 12, frames }, null, 2) + "\n",
  );

  if (existsSync(outDir)) {
    const expected = readdirSync(stageDir).sort();
    const actual = readdirSync(outDir).sort();
    const identical = expected.length === actual.length &&
      expected.every((name, index) =>
        name === actual[index] && readFileSync(join(stageDir, name))
          .equals(readFileSync(join(outDir, name))));
    if (!identical) {
      throw new Error(`the output directory exists with different content: ${outDir}`);
    }
    rmSync(stageDir, { recursive: true, force: true });
    action = "verified";
  } else {
    const published = spawnSync("python3", [publishTool, stageDir, outDir], {
      encoding: "utf8",
      stdio: ["ignore", "ignore", "pipe"],
    });
    if (published.status !== 0) {
      throw new Error(
        `could not publish the demo without replacement: ${published.stderr.trim()}`,
      );
    }
  }
} catch (error) {
  if (existsSync(stageDir)) rmSync(stageDir, { recursive: true, force: true });
  throw error;
}
console.log(`${action} ${FRAMES} frames + manifest.json at ${outDir}`);
