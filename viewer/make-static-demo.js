#!/usr/bin/env node
// Generate a deterministic, project-owned SPLAT fixture. This keeps the
// packaged desktop viewer useful and exercises Spark's .splat decoder without
// redistributing a third-party capture whose license is unspecified.
//
// Common SPLAT layout: position float3, linear scale float3, RGBA u8, and
// quaternion u8x4 mapped from [-1, 1] to [0, 255] (32 bytes per splat).
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";

const output = process.argv[2] || "public/splats/generated/wave.splat";
const side = Number(process.argv[3] || 64);
if (!Number.isInteger(side) || side < 2 || side > 1024) {
  throw new Error("side must be an integer in [2, 1024]");
}

const count = side * side;
const bytes = Buffer.alloc(count * 32);
let offset = 0;
const putFloat = (value) => {
  bytes.writeFloatLE(value, offset);
  offset += 4;
};
const putByte = (value) => {
  bytes.writeUInt8(Math.max(0, Math.min(255, Math.round(value))), offset++);
};

for (let row = 0; row < side; row++) {
  for (let column = 0; column < side; column++) {
    const x = (column / (side - 1) - 0.5) * 2.4;
    const y = (row / (side - 1) - 0.5) * 2.4;
    const z = 0.28 * Math.sin(x * 3.2) * Math.cos(y * 2.6);
    putFloat(x);
    putFloat(y);
    putFloat(z);
    putFloat(0.032);
    putFloat(0.032);
    putFloat(0.012);
    const color = 0.5 + 0.5 * Math.sin(x * 2.4 + y);
    putByte(45 + color * 190);
    putByte(80 + (1 - color) * 130);
    putByte(235 - color * 70);
    putByte(240);
    // Identity quaternion (wxyz): [1, 0, 0, 0].
    putByte(255);
    putByte(128);
    putByte(128);
    putByte(128);
  }
}

mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, bytes);
console.log(`wrote ${count} splats to ${output}`);
