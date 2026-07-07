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
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const outDir = process.argv[2] || "public/splats/4d/wave";
const FRAMES = Number(process.argv[3] || 24);
const SIDE = Number(process.argv[4] || 40);
mkdirSync(outDir, { recursive: true });

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
      put(0); put(0); put(1);          // normal
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
for (let i = 0; i < FRAMES; i++) {
  const name = `time_${String(i).padStart(5, "0")}.ply`;
  writeFileSync(join(outDir, name), frame(i));
  frames.push(name);
}
writeFileSync(
  join(outDir, "manifest.json"),
  JSON.stringify({ fps: 12, frames }, null, 2) + "\n"
);
console.log(`wrote ${FRAMES} frames + manifest.json to ${outDir}`);
