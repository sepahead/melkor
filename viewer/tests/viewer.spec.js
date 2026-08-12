import { test, expect } from "@playwright/test";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { spawnSync } from "node:child_process";
import { deflateRawSync, gzipSync } from "node:zlib";
import { validateLossReport } from "../loss-report-validator.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const SHOT_DIR = join(__dirname, "..", "screenshots");
const LOCAL_SPLAT = join(__dirname, "..", "public", "splats", "generated", "wave.splat");
const IS_CI = /^(1|true)$/i.test(process.env.CI ?? "");
const FULL_RENDER = !IS_CI || /^(1|true)$/i.test(process.env.VIEWER_FULL_RENDER ?? "");
const DEFAULT_TEST_SCENE = IS_CI ? "wave-static" : null;
const RENDER_MASK_SELECTORS = [
  "#hud", "#scenes", "#bar", "#tl", "#overlay .card", "#dropTarget .card",
];
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

test.describe("viewer asset tools", () => {
  test("static demo generation is idempotent and never replaces drift", () => {
    const root = mkdtempSync(join(tmpdir(), "melkor-static-demo-test-"));
    try {
      const output = join(root, "wave.splat");
      const command = [join(__dirname, "..", "make-static-demo.js"), output, "2"];
      const first = spawnSync(process.execPath, command, { encoding: "utf8" });
      expect(first.status, first.stderr).toBe(0);
      const second = spawnSync(process.execPath, command, { encoding: "utf8" });
      expect(second.status, second.stderr).toBe(0);
      expect(second.stdout).toContain("verified 4 splats");

      writeFileSync(output, "drift\n");
      const drift = spawnSync(process.execPath, command, { encoding: "utf8" });
      expect(drift.status).not.toBe(0);
      expect(drift.stderr).toContain("different content");
      expect(readFileSync(output, "utf8")).toBe("drift\n");
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });

  test("4D loss-report validation is closed and policy-aware", () => {
    const report = {
      schema_version: 1,
      input: { format: "ply", profile: "ply:graphdeco-3dgs-v1" },
      output: { format: "spz", profile: "spz:spz-v1-v3" },
      items: [{
        code: "LOSS_QUANTIZATION_APPLIED",
        severity: "warning",
        source_feature: "32-bit source attributes",
        target_constraint: "SPZ quantized fields",
        affected_splats: 1,
        remediation: "Use a lossless target.",
      }],
      approved_codes: [],
    };
    expect(validateLossReport(report, {
      inputFormat: "ply",
      outputFormat: "spz",
    })).toBe(report);

    expect(() => validateLossReport({ ...report, extra: true })).toThrow("unknown or missing");
    expect(() => validateLossReport({
      ...report,
      items: [{ ...report.items[0], severity: "info" }],
    })).toThrow("severity is invalid");
    expect(() => validateLossReport({
      ...report,
      items: [{
        ...report.items[0],
        code: "LOSS_SCALE_CLAMPED",
        severity: "severe",
      }],
    })).toThrow("unapproved severe loss");
    expect(() => validateLossReport({
      ...report,
      items: [{ ...report.items[0], severity: "fatal" }],
    })).toThrow("fatal loss");
  });

  test("4D demo generation is idempotent and never replaces drift", () => {
    const root = mkdtempSync(join(tmpdir(), "melkor-demo-test-"));
    try {
      const output = join(root, "wave");
      const command = [join(__dirname, "..", "make-4d-demo.js"), output, "2", "2"];
      const first = spawnSync(process.execPath, command, { encoding: "utf8" });
      expect(first.status, first.stderr).toBe(0);
      const second = spawnSync(process.execPath, command, { encoding: "utf8" });
      expect(second.status, second.stderr).toBe(0);
      expect(second.stdout).toContain("verified 2 frames");

      writeFileSync(join(output, "time_00000.ply"), "drift\n");
      const drift = spawnSync(process.execPath, command, { encoding: "utf8" });
      expect(drift.status).not.toBe(0);
      expect(drift.stderr).toContain("different content");
      expect(readFileSync(join(output, "time_00000.ply"), "utf8")).toBe("drift\n");
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });

  test("4D packer publishes a complete local sequence", () => {
    const root = mkdtempSync(join(tmpdir(), "melkor-pack-test-"));
    try {
      const source = join(root, "frames");
      const output = join(root, "packed");
      mkdirSync(source);
      writeFileSync(join(source, "time_00000.ply"), "fixture\n");
      const result = spawnSync(
        process.execPath,
        [join(__dirname, "..", "pack-4d.js"), source, "--out", output],
        { encoding: "utf8" },
      );
      expect(result.status, result.stderr).toBe(0);
      const manifest = JSON.parse(readFileSync(join(output, "manifest.json"), "utf8"));
      expect(manifest).toEqual({ fps: 12, frames: ["time_00000.ply"] });
      expect(readFileSync(join(output, "time_00000.ply"), "utf8")).toBe("fixture\n");
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });

  test("4D packer never replaces an unrelated directory", () => {
    const root = mkdtempSync(join(tmpdir(), "melkor-pack-guard-"));
    try {
      const source = join(root, "frames");
      const output = join(root, "unrelated");
      mkdirSync(source);
      mkdirSync(output);
      writeFileSync(join(source, "time_00000.ply"), "fixture\n");
      writeFileSync(join(output, "keep.txt"), "keep\n");
      const result = spawnSync(
        process.execPath,
        [join(__dirname, "..", "pack-4d.js"), source, "--out", output],
        { encoding: "utf8" },
      );
      expect(result.status).toBe(2);
      expect(result.stderr).toContain("already exists");
      expect(readFileSync(join(output, "keep.txt"), "utf8")).toBe("keep\n");
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });

  test("4D packer never replaces an owned directory", () => {
    const root = mkdtempSync(join(tmpdir(), "melkor-pack-owned-"));
    try {
      const source = join(root, "frames");
      const output = join(root, "packed");
      mkdirSync(source);
      mkdirSync(output);
      writeFileSync(join(source, "time_00000.ply"), "new\n");
      writeFileSync(
        join(output, "manifest.json"),
        JSON.stringify({ fps: 12, frames: ["time_00000.ply"] }),
      );
      writeFileSync(join(output, "time_00000.ply"), "old\n");
      const result = spawnSync(
        process.execPath,
        [join(__dirname, "..", "pack-4d.js"), source, "--out", output],
        { encoding: "utf8" },
      );
      expect(result.status).toBe(2);
      expect(result.stderr).toContain("already exists");
      expect(readFileSync(join(output, "time_00000.ply"), "utf8")).toBe("old\n");
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });

  test("4D packer rejects source overlap through a symbolic-link parent", () => {
    const root = mkdtempSync(join(tmpdir(), "melkor-pack-link-"));
    try {
      const source = join(root, "frames");
      const alias = join(root, "alias");
      mkdirSync(source);
      writeFileSync(join(source, "time_00000.ply"), "fixture\n");
      symlinkSync(source, alias, "dir");
      const result = spawnSync(
        process.execPath,
        [join(__dirname, "..", "pack-4d.js"), source, "--out", join(alias, "packed")],
        { encoding: "utf8" },
      );
      expect(result.status).toBe(2);
      expect(result.stderr).toContain("after path resolution");
      expect(existsSync(join(source, "packed"))).toBe(false);
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
});

async function served(request, file) {
  const r = await request.fetch(`/public/splats/${file}`, { method: "HEAD" });
  return r.status() === 200;
}

function zipWithDeclaredSizes(compressedBytes, expandedBytes) {
  const name = Buffer.from("meta.json", "ascii");
  const local = Buffer.alloc(30 + name.length + compressedBytes);
  local.writeUInt32LE(0x04034b50, 0);
  local.writeUInt16LE(20, 4);
  local.writeUInt16LE(8, 8);
  local.writeUInt32LE(compressedBytes, 18);
  local.writeUInt32LE(expandedBytes, 22);
  local.writeUInt16LE(name.length, 26);
  name.copy(local, 30);

  const central = Buffer.alloc(46 + name.length);
  central.writeUInt32LE(0x02014b50, 0);
  central.writeUInt16LE(20, 4);
  central.writeUInt16LE(20, 6);
  central.writeUInt16LE(8, 10);
  central.writeUInt32LE(compressedBytes, 20);
  central.writeUInt32LE(expandedBytes, 24);
  central.writeUInt16LE(name.length, 28);
  name.copy(central, 46);

  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(1, 8);
  eocd.writeUInt16LE(1, 10);
  eocd.writeUInt32LE(central.length, 12);
  eocd.writeUInt32LE(local.length, 16);
  return Buffer.concat([local, central, eocd]);
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc & 1) !== 0 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function deflatedZipWithExpandedSize(payload, expandedBytes) {
  const name = Buffer.from("meta.json", "ascii");
  const compressed = deflateRawSync(payload);
  const checksum = crc32(payload);
  const local = Buffer.alloc(30 + name.length + compressed.length);
  local.writeUInt32LE(0x04034b50, 0);
  local.writeUInt16LE(20, 4);
  local.writeUInt16LE(8, 8);
  local.writeUInt32LE(checksum, 14);
  local.writeUInt32LE(compressed.length, 18);
  local.writeUInt32LE(expandedBytes, 22);
  local.writeUInt16LE(name.length, 26);
  name.copy(local, 30);
  compressed.copy(local, 30 + name.length);

  const central = Buffer.alloc(46 + name.length);
  central.writeUInt32LE(0x02014b50, 0);
  central.writeUInt16LE(20, 4);
  central.writeUInt16LE(20, 6);
  central.writeUInt16LE(8, 10);
  central.writeUInt32LE(checksum, 16);
  central.writeUInt32LE(compressed.length, 20);
  central.writeUInt32LE(expandedBytes, 24);
  central.writeUInt16LE(name.length, 28);
  name.copy(central, 46);

  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(1, 8);
  eocd.writeUInt16LE(1, 10);
  eocd.writeUInt32LE(central.length, 12);
  eocd.writeUInt32LE(local.length, 16);
  return Buffer.concat([local, central, eocd]);
}

function storedZip(entries) {
  const localParts = [];
  const centralParts = [];
  let localOffset = 0;
  for (const [filename, payload] of entries) {
    const name = Buffer.from(filename, "utf8");
    const bytes = Buffer.from(payload);
    const checksum = crc32(bytes);
    const local = Buffer.alloc(30 + name.length + bytes.length);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(0, 8);
    local.writeUInt32LE(checksum, 14);
    local.writeUInt32LE(bytes.length, 18);
    local.writeUInt32LE(bytes.length, 22);
    local.writeUInt16LE(name.length, 26);
    name.copy(local, 30);
    bytes.copy(local, 30 + name.length);
    localParts.push(local);

    const central = Buffer.alloc(46 + name.length);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(0, 10);
    central.writeUInt32LE(checksum, 16);
    central.writeUInt32LE(bytes.length, 20);
    central.writeUInt32LE(bytes.length, 24);
    central.writeUInt16LE(name.length, 28);
    central.writeUInt32LE(localOffset, 42);
    name.copy(central, 46);
    centralParts.push(central);
    localOffset += local.length;
  }
  const central = Buffer.concat(centralParts);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(central.length, 12);
  eocd.writeUInt32LE(localOffset, 16);
  return Buffer.concat([...localParts, central, eocd]);
}

function pngHeader(width, height) {
  const bytes = Buffer.alloc(33);
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(bytes);
  bytes.writeUInt32BE(13, 8);
  bytes.write("IHDR", 12, "ascii");
  bytes.writeUInt32BE(width, 16);
  bytes.writeUInt32BE(height, 20);
  return bytes;
}

function canonicalPly(profileMarker = true) {
  const names = [
    "x", "y", "z", "scale_x", "scale_y", "scale_z",
    "rotation_x", "rotation_y", "rotation_z", "rotation_w", "opacity",
    "sh_0_r", "sh_0_g", "sh_0_b",
  ];
  const marker = profileMarker ? "comment melkor_profile melkor-canonical-v1\n" : "";
  const header = Buffer.from(
    "ply\nformat binary_little_endian 1.0\n" + marker + "element vertex 1\n" +
    names.map((name) => `property float ${name}\n`).join("") + "end_header\n",
  );
  const record = Buffer.alloc(names.length * 4);
  [0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0]
    .forEach((value, index) => record.writeFloatLE(value, index * 4));
  return Buffer.concat([header, record]);
}

function graphdecoPly({
  marker = true,
  position = [0, 0, 0],
  rotation = [1, 0, 0, 0],
  scale = [0, 0, 0],
} = {}) {
  const names = [
    "x", "y", "z", "scale_0", "scale_1", "scale_2",
    "rot_0", "rot_1", "rot_2", "rot_3", "opacity",
    "f_dc_0", "f_dc_1", "f_dc_2",
  ];
  const profile = marker ? "comment melkor_profile ply:graphdeco-3dgs-v1\n" : "";
  const header = Buffer.from(
    "ply\nformat binary_little_endian 1.0\n" + profile + "element vertex 1\n" +
    names.map((name) => `property float ${name}\n`).join("") + "end_header\n",
  );
  const record = Buffer.alloc(names.length * 4);
  [...position, ...scale, ...rotation, 0, 0, 0, 0]
    .forEach((value, index) => record.writeFloatLE(value, index * 4));
  return Buffer.concat([header, record]);
}

function graphdecoDoubleColorPly(color = [0, 0, 0]) {
  const floatNames = [
    "x", "y", "z", "scale_0", "scale_1", "scale_2",
    "rot_0", "rot_1", "rot_2", "rot_3", "opacity",
  ];
  const header = Buffer.from(
    "ply\nformat binary_little_endian 1.0\nelement vertex 1\n" +
    floatNames.map((name) => `property float ${name}\n`).join("") +
    ["f_dc_0", "f_dc_1", "f_dc_2"]
      .map((name) => `property double ${name}\n`).join("") +
    "end_header\n",
  );
  const record = Buffer.alloc(floatNames.length * 4 + color.length * 8);
  [0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0]
    .forEach((value, index) => record.writeFloatLE(value, index * 4));
  color.forEach((value, index) => record.writeDoubleLE(
    value,
    floatNames.length * 4 + index * 8,
  ));
  return Buffer.concat([header, record]);
}

function graphdecoPlyWithExtraProperties(extraNames) {
  const names = [
    "x", "y", "z", "scale_0", "scale_1", "scale_2",
    "rot_0", "rot_1", "rot_2", "rot_3", "opacity",
    "f_dc_0", "f_dc_1", "f_dc_2", ...extraNames,
  ];
  const header = Buffer.from(
    "ply\nformat binary_little_endian 1.0\nelement vertex 1\n" +
    names.map((name) => `property float ${name}\n`).join("") + "end_header\n",
  );
  const record = Buffer.alloc(names.length * 4);
  record.writeFloatLE(1, names.indexOf("rot_0") * 4);
  return Buffer.concat([header, record]);
}

function compressedPly(packedRotation) {
  const chunkNames = [
    "min_x", "min_y", "min_z", "max_x", "max_y", "max_z",
    "min_scale_x", "min_scale_y", "min_scale_z",
    "max_scale_x", "max_scale_y", "max_scale_z",
  ];
  const packedNames = [
    "packed_position", "packed_rotation", "packed_scale", "packed_color",
  ];
  const header = Buffer.from(
    "ply\nformat binary_little_endian 1.0\nelement chunk 1\n" +
    chunkNames.map((name) => `property float ${name}\n`).join("") +
    "element vertex 1\n" +
    packedNames.map((name) => `property uint ${name}\n`).join("") +
    "end_header\n",
  );
  const records = Buffer.alloc(chunkNames.length * 4 + packedNames.length * 4);
  records.writeUInt32LE(packedRotation >>> 0, chunkNames.length * 4 + 4);
  return Buffer.concat([header, records]);
}

function antiSplat({ position = [0, 0, 0], scale = [1, 1, 1], rotation = [255, 128, 128, 128] } = {}) {
  const record = Buffer.alloc(32);
  [...position, ...scale].forEach((value, index) => record.writeFloatLE(value, index * 4));
  Buffer.from([255, 255, 255, 255, ...rotation]).copy(record, 24);
  return record;
}

function oneSplatSpz(version, rotation, options = {}) {
  const positionBytes = version === 1 ? 6 : 9;
  const rotationBytes = version >= 3 ? 4 : 3;
  const hasLod = ((options.flags ?? 0) & 0x80) !== 0;
  const decoded = Buffer.alloc(16 + positionBytes + 1 + 3 + 3 + rotationBytes +
    (hasLod ? 6 : 0));
  decoded.writeUInt32LE(0x5053474e, 0);
  decoded.writeUInt32LE(version, 4);
  decoded.writeUInt32LE(1, 8);
  decoded.writeUInt8(0, 12);
  decoded.writeUInt8(options.fractionalBits ?? (version === 1 ? 0 : 12), 13);
  decoded.writeUInt8(options.flags ?? 0, 14);
  if (options.position) Buffer.from(options.position).copy(decoded, 16);
  const rotationOffset = 16 + positionBytes + 1 + 3 + 3;
  Buffer.from(rotation).copy(decoded, rotationOffset);
  if (hasLod) {
    decoded.writeUInt16LE(options.lodCount ?? 0, rotationOffset + rotationBytes);
    decoded.writeUInt32LE(options.lodStart ?? 0, rotationOffset + rotationBytes + 2);
  }
  return gzipSync(decoded);
}

function oneSectionKsplat({
  compression = 0,
  declaredCount = 1,
  position = [0, 0, 0],
  rotation = [1, 0, 0, 0],
  scale = [1, 1, 1],
  sh = [],
} = {}) {
  const headerBytes = 4096;
  const sectionBytes = 1024;
  const shDegree = sh.length === 0 ? 0 : sh.length === 9 ? 1 : 0xffff;
  const shBytes = compression === 0 ? sh.length * 4 : sh.length * 2;
  const recordBytes = (compression === 0 ? 44 : 24) + shBytes;
  const bucketBytes = compression === 0 ? 0 : 12;
  const bytes = Buffer.alloc(headerBytes + sectionBytes + bucketBytes + recordBytes);
  bytes.writeUInt8(1, 1);
  bytes.writeUInt32LE(1, 4);
  bytes.writeUInt32LE(declaredCount, 16);
  bytes.writeUInt16LE(compression, 20);

  const section = headerBytes;
  bytes.writeUInt32LE(1, section);
  bytes.writeUInt32LE(1, section + 4);
  bytes.writeUInt16LE(bucketBytes, section + 20);
  bytes.writeUInt16LE(shDegree, section + 40);
  let record = headerBytes + sectionBytes;
  if (compression > 0) {
    bytes.writeUInt32LE(1, section + 8);
    bytes.writeUInt32LE(1, section + 12);
    bytes.writeFloatLE(1, section + 16);
    bytes.writeUInt32LE(1, section + 32);
    position.forEach((value, axis) => bytes.writeFloatLE(value, record + axis * 4));
    record += bucketBytes;
    [32_767, 32_767, 32_767].forEach((value, axis) =>
      bytes.writeUInt16LE(value, record + axis * 2));
    scale.forEach((value, axis) => bytes.writeUInt16LE(value, record + 6 + axis * 2));
    rotation.forEach((value, component) =>
      bytes.writeUInt16LE(value, record + 12 + component * 2));
    sh.forEach((value, component) =>
      bytes.writeUInt16LE(value, record + 24 + component * 2));
  } else {
    position.forEach((value, axis) => bytes.writeFloatLE(value, record + axis * 4));
    scale.forEach((value, axis) => bytes.writeFloatLE(value, record + 12 + axis * 4));
    rotation.forEach((value, component) =>
      bytes.writeFloatLE(value, record + 24 + component * 4));
    bytes.fill(0xff, record + 40, record + 44);
    sh.forEach((value, component) =>
      bytes.writeFloatLE(value, record + 44 + component * 4));
  }
  return bytes;
}

// Sample a compositor screenshot rather than WebGL's transient drawing buffer.
// With preserveDrawingBuffer=false, readPixels can legally observe a cleared
// buffer even while Chromium displays a valid frame. The screenshot measures
// what the user actually sees: coverage, tonal variance, and a spatial hash.
async function pixelStats(page) {
  // Capture the actual compositor output without changing page styles. On
  // SwiftShader, hiding a backdrop-filter can invalidate the WebGL layer in
  // the same frame and expose the cleared drawing buffer. Instead, mask the
  // exact UI/card rectangles during pixel analysis; a uniform error backdrop
  // remains part of the sample and cannot make a blank scene look rendered.
  const capture = await page.evaluate((selectors) => ({
    width: window.innerWidth,
    height: window.innerHeight,
    masks: selectors.flatMap((selector) => {
      const element = document.querySelector(selector);
      if (!element) return [];
      const style = getComputedStyle(element);
      const rect = element.getBoundingClientRect();
      if (style.display === "none" || style.visibility === "hidden" ||
          rect.width <= 0 || rect.height <= 0) return [];
      const margin = 8; // cover borders and the error backdrop's 2px blur bleed
      return [{
        left: Math.max(0, rect.left - margin),
        top: Math.max(0, rect.top - margin),
        right: Math.min(window.innerWidth, rect.right + margin),
        bottom: Math.min(window.innerHeight, rect.bottom + margin),
      }];
    }),
  }), RENDER_MASK_SELECTORS);
  const png = await page.screenshot({ type: "png" });
  const dataUrl = `data:image/png;base64,${png.toString("base64")}`;
  return await page.evaluate(async ({ src, capture }) => {
    const image = new Image();
    image.src = src;
    await image.decode();
    const canvas = document.createElement("canvas");
    canvas.width = image.naturalWidth;
    canvas.height = image.naturalHeight;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    context.drawImage(image, 0, 0);
    const buf = context.getImageData(0, 0, canvas.width, canvas.height).data;
    const scaleX = canvas.width / capture.width;
    const scaleY = canvas.height / capture.height;
    let nonBg = 0, n = 0, sum = 0, sum2 = 0, signature = 2166136261;
    for (let i = 0; i < buf.length; i += 4 * 131) {
      const pixel = i / 4;
      const x = (pixel % canvas.width) / scaleX;
      const y = Math.floor(pixel / canvas.width) / scaleY;
      if (capture.masks.some((rect) =>
        x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom)) continue;
      const r = buf[i], gg = buf[i + 1], b = buf[i + 2];
      const lum = 0.299 * r + 0.587 * gg + 0.114 * b;
      sum += lum; sum2 += lum * lum; n++;
      if (Math.abs(r - 11) + Math.abs(gg - 12) + Math.abs(b - 16) > 24) nonBg++;
      signature ^= (r | (gg << 8) | (b << 16)) ^ i;
      signature = Math.imul(signature, 16777619) >>> 0;
    }
    if (n === 0) throw new Error("render capture left no unmasked pixels");
    const mean = sum / n;
    return {
      nonBgFrac: nonBg / n,
      lumStd: Math.sqrt(Math.max(sum2 / n - mean * mean, 0)),
      signature: signature.toString(16).padStart(8, "0"),
    };
  }, { src: dataUrl, capture });
}

async function settledPixelStats(page, timeoutMs = 5_000) {
  const deadline = Date.now() + timeoutMs;
  let stats;
  do {
    stats = await pixelStats(page);
    if (stats.nonBgFrac > 0.001 && stats.lumStd > 0.5) return stats;
    await page.waitForTimeout(100);
  } while (Date.now() < deadline);
  return stats;
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

test.describe("Melkor Viewer", () => {
  test("serves splat assets", async ({ request }) => {
    for (const s of SCENES) {
      const ok = await served(request, s.file);
      if (s.optional && !ok) { test.info().annotations.push({ type: "skip-asset", description: s.file }); continue; }
      expect(ok, `${s.file} served (200)`).toBe(true);
    }
  });

  test("development server fails closed on malformed and private paths", async ({ request }) => {
    const index = await request.get("/index.html");
    expect(index.headers()["content-security-policy"]).toContain("frame-ancestors 'none'");
    expect(index.headers()["content-security-policy"]).not.toContain("unsafe-inline");
    expect(index.headers()["x-content-type-options"]).toBe("nosniff");
    expect(index.headers()["x-frame-options"]).toBe("DENY");
    const malformed = await request.fetch("/%");
    expect(malformed.status()).toBe(400);
    expect((await request.fetch("/package.json")).status()).toBe(404);
    expect((await request.fetch("/bootstrap.js")).status()).toBe(200);
    expect((await request.fetch("/local-file-validator.js")).status()).toBe(200);
    expect((await request.fetch("/local-file-validator-worker.js")).status()).toBe(200);
    expect((await request.fetch("/favicon.png")).status()).toBe(200);
    expect((await request.fetch("/src-tauri/icons/32x32.png")).status()).toBe(404);
    expect((await request.fetch("/viewer.css")).status()).toBe(200);
    expect((await request.fetch("/viewer.js")).status()).toBe(200);
    for (const path of [
      "/vendor/../package.json",
      "/vendor/..%2Fpackage.json",
      "/public/..%2Fsrc-tauri%2FCargo.toml",
      "/public/%2e%2e%2fsrc-tauri%2fCargo.toml",
    ]) {
      expect((await request.fetch(path)).status(), path).not.toBe(200);
    }
    expect((await request.post("/index.html")).status()).toBe(405);

    const link = join(__dirname, "..", "public", "__viewer-private-link__");
    try {
      symlinkSync("../package.json", link);
      expect((await request.fetch("/public/__viewer-private-link__")).status()).not.toBe(200);
    } finally {
      rmSync(link, { force: true });
    }
  });

  test("a missing runtime module produces a visible fatal state", async ({ page }) => {
    await page.route("**/vendor/spark.module.js", (route) =>
      route.fulfill({ status: 404, body: "missing" }));
    await page.goto("/index.html");
    await expect(page.getByRole("alertdialog")).toContainText(
      "The viewer runtime could not start",
    );
    await expect(page.getByRole("button", { name: "Reload" })).toBeFocused();
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeHidden();
    expect(await page.evaluate(() => window.__viewer.state.error)).toContain(
      "viewer runtime",
    );
  });

  test("a missing application module produces a visible fatal state", async ({ page }) => {
    await page.route("**/viewer.js", (route) =>
      route.fulfill({ status: 404, body: "missing" }));
    await page.goto("/index.html");
    await expect(page.getByRole("alertdialog")).toContainText(
      "The viewer application could not start",
    );
    await expect(page.getByRole("button", { name: "Reload" })).toBeFocused();
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeHidden();
  });

  test("no bundled asset leaves a usable local-file recovery path", async ({ page }) => {
    await page.route("**/public/scene-index.json", (route) =>
      route.fulfill({ status: 404, body: "missing" }));
    await page.route("**/public/splats/**", (route) => {
      if (route.request().method() === "HEAD") {
        return route.fulfill({ status: 404, body: "missing" });
      }
      return route.continue();
    });
    await boot(page, null);
    await expect(page.locator("button[data-scene]")).toHaveCount(0);
    await expect(page.getByRole("alertdialog")).toContainText("No bundled scene is available");
    await expect(page.getByRole("button", { name: "Open local splat", exact: true })).toBeVisible();

    await page.locator("#localFileInput").setInputFiles({
      name: "not-a-splat.txt",
      mimeType: "text/plain",
      buffer: Buffer.from("not a splat"),
    });
    await expect(page.getByRole("alertdialog")).toContainText("Unsupported file type");
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeHidden();
    const failure = await page.evaluate(async () => {
      const started = performance.now();
      try {
        await window.__viewer.waitRendered(2_000);
        return null;
      } catch (error) {
        return { elapsed: performance.now() - started, message: String(error) };
      }
    });
    expect(failure?.message).toContain("Unsupported file type");
    expect(failure?.elapsed).toBeLessThan(1_000);

    await page.locator("#localFileInput").setInputFiles(LOCAL_SPLAT);
    const stats = await page.evaluate(() => window.__viewer.waitRendered(30_000));
    expect(stats.scene).toBe("local-file");
    expect(stats.splatCount).toBeGreaterThanOrEqual(4_000);
  });

  test("controls fit and do not overlap at supported small sizes", async ({ page }) => {
    const viewports = [
      { width: 800, height: 400 },
      { width: 667, height: 375 },
      { width: 640, height: 360 },
      { width: 375, height: 667 },
      { width: 320, height: 568 },
    ];
    await page.setViewportSize(viewports[0]);
    await boot(page, "wave-static");

    for (const viewport of viewports) {
      await page.setViewportSize(viewport);
      const layout = await page.evaluate(async () => {
        const timeline = document.getElementById("tl");
        timeline.hidden = false;
        await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        const ids = ["hud", "scenes", "tl", "bar"];
        const regions = ids.map((id) => {
          const rect = document.getElementById(id).getBoundingClientRect();
          return { id, left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom };
        });
        const toolbar = document.getElementById("bar").getBoundingClientRect();
        const toolbarButtons = [...document.querySelectorAll("#bar button")].map((button) => {
          const rect = button.getBoundingClientRect();
          return { left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom };
        });
        return { regions, toolbar: { left: toolbar.left, right: toolbar.right }, toolbarButtons };
      });

      for (const rect of layout.regions) {
        expect(rect.left, `${rect.id} starts inside ${viewport.width}x${viewport.height}`)
          .toBeGreaterThanOrEqual(0);
        expect(rect.right, `${rect.id} ends inside ${viewport.width}x${viewport.height}`)
          .toBeLessThanOrEqual(viewport.width);
        expect(rect.top, `${rect.id} starts inside ${viewport.width}x${viewport.height}`)
          .toBeGreaterThanOrEqual(0);
        expect(rect.bottom, `${rect.id} ends inside ${viewport.width}x${viewport.height}`)
          .toBeLessThanOrEqual(viewport.height);
      }
      for (let first = 0; first < layout.regions.length; first++) {
        for (let second = first + 1; second < layout.regions.length; second++) {
          const a = layout.regions[first];
          const b = layout.regions[second];
          const overlapWidth = Math.max(0, Math.min(a.right, b.right) - Math.max(a.left, b.left));
          const overlapHeight = Math.max(0, Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top));
          expect(overlapWidth * overlapHeight, `${a.id} and ${b.id} do not overlap at ${viewport.width}x${viewport.height}`)
            .toBeLessThan(1);
        }
      }
      for (const button of layout.toolbarButtons) {
        expect(button.left).toBeGreaterThanOrEqual(layout.toolbar.left);
        expect(button.right).toBeLessThanOrEqual(layout.toolbar.right);
      }
    }
  });

  test("camera controls and help expose complete keyboard access", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));

    const toolbar = page.getByRole("toolbar", { name: "Camera controls" });
    await expect(toolbar).toBeVisible();
    await expect(toolbar).toHaveAttribute("aria-orientation", "horizontal");
    await expect(toolbar.locator("button[tabindex='0']")).toHaveCount(1);
    await expect(page.locator("#localOpen")).not.toHaveAttribute("aria-pressed");
    const canvas = page.getByRole("application", { name: "Interactive Gaussian splat viewport" });
    await expect(canvas).toHaveAttribute("aria-describedby", "canvas-description");
    await expect(page.locator("#canvas-description")).toContainText("camera toolbar");

    await page.getByRole("button", { name: "Side" }).click();
    await expect(page.getByRole("button", { name: "Side" })).toHaveAttribute("aria-pressed", "true");
    await expect(page.getByRole("button", { name: "Front" })).toHaveAttribute("aria-pressed", "false");
    await page.getByRole("button", { name: "Front" }).focus();
    await expect(toolbar.locator("button[tabindex='0']")).toHaveText("Front");
    await page.keyboard.press("ArrowLeft");
    await expect(page.getByRole("button", { name: "Help" })).toBeFocused();
    await page.keyboard.press("Home");
    await expect(page.getByRole("button", { name: "Front" })).toBeFocused();
    await page.keyboard.press("End");
    await expect(page.getByRole("button", { name: "Help" })).toBeFocused();
    await page.keyboard.press("ArrowRight");
    await expect(page.getByRole("button", { name: "Front" })).toBeFocused();
    await page.keyboard.press("ArrowRight");
    await expect(page.getByRole("button", { name: "Side" })).toBeFocused();
    await expect(toolbar.locator("button[tabindex='0']")).toHaveCount(1);
    await expect(toolbar.locator("button[tabindex='0']")).toHaveText("Side");
    await page.keyboard.press("Tab");
    expect(await page.evaluate(() => document.activeElement?.closest("#bar") === null)).toBe(true);

    await page.getByRole("button", { name: "Help" }).click();
    const help = page.getByRole("dialog", { name: "Viewer controls" });
    await expect(help).toBeVisible();
    await expect(help).toContainText("WASD / arrows");
    await expect(help).toContainText("Timeline arrows");
    await expect(help).toContainText("Escape");
    await page.keyboard.press("Escape");
    await expect(help).toBeHidden();
    await expect(page.getByRole("button", { name: "Help" })).toBeFocused();
  });

  test("error dialog traps focus and restores the prior control", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const front = page.getByRole("button", { name: "Front" });
    await front.focus();

    await page.locator("#localFileInput").setInputFiles({
      name: "invalid.txt",
      mimeType: "text/plain",
      buffer: Buffer.from("not a splat"),
    });
    const dialog = page.getByRole("alertdialog");
    await expect(dialog).toBeVisible();
    await expect(dialog).toHaveAttribute("aria-modal", "true");
    expect(await page.evaluate(() => ["app", "hud", "scenes", "bar", "tl"]
      .every((id) => document.getElementById(id).inert))).toBe(true);

    const retry = page.getByRole("button", { name: "Choose another file" });
    const dismiss = page.getByRole("button", { name: "Dismiss" });
    await expect(retry).toBeFocused();
    await page.keyboard.press("Tab");
    await expect(dismiss).toBeFocused();
    await page.keyboard.press("Tab");
    await expect(retry).toBeFocused();
    await page.keyboard.press("Shift+Tab");
    await expect(dismiss).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
    await expect(front).toBeFocused();
    expect(await page.evaluate(() => ["app", "hud", "scenes", "bar", "tl"]
      .every((id) => !document.getElementById(id).inert))).toBe(true);
  });

  test("a completed background load cannot close a newer file error", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));

    let releaseRequest;
    const requestGate = new Promise((resolve) => { releaseRequest = resolve; });
    await page.route("**/public/splats/generated/wave.splat", async (route) => {
      const response = await route.fetch();
      await requestGate;
      await route.fulfill({ response });
    });
    await page.evaluate(() => { void window.__viewer.load("wave-static"); });
    await page.waitForFunction(() => window.__viewer.state.loading === true);

    await page.locator("#localFileInput").setInputFiles({
      name: "newer-invalid.txt",
      mimeType: "text/plain",
      buffer: Buffer.from("not a splat"),
    });
    await expect(page.getByRole("alertdialog")).toContainText("Unsupported file type");

    releaseRequest();
    await page.waitForFunction(() => window.__viewer.state.loading === false);
    await expect(page.getByRole("alertdialog")).toContainText("Unsupported file type");
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeVisible();
  });

  test("a later scene choice cancels pending local validation", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const spz = Buffer.alloc(36);
    spz.writeUInt32LE(0x5053474e, 0);
    spz.writeUInt32LE(3, 4);
    spz.writeUInt32LE(1, 8);
    spz.writeUInt8(12, 13);
    const encoded = gzipSync(spz).toString("base64");

    const stats = await page.evaluate(async (base64) => {
      const bytes = Uint8Array.from(atob(base64), (value) => value.charCodeAt(0));
      const file = new File([bytes], "delayed.spz", { type: "application/octet-stream" });
      let releaseValidation;
      const validationGate = new Promise((resolve) => { releaseValidation = resolve; });
      const originalStream = file.stream.bind(file);
      file.stream = () => originalStream().pipeThrough(new TransformStream({
        async transform(chunk, controller) {
          await validationGate;
          controller.enqueue(chunk);
        },
      }));
      const staleOpen = window.__viewer.openFile(file);
      const laterScene = window.__viewer.load("wave-static");
      releaseValidation();
      await Promise.all([staleOpen, laterScene]);
      await window.__viewer.waitRendered(30_000);
      return {
        stats: window.__viewer.getStats(),
        localScenes: window.__viewer.scenes.filter((scene) => scene.local),
      };
    }, encoded);
    expect(stats.stats.scene).toBe("wave-static");
    expect(stats.stats.source).toBe("bundled");
    expect(stats.localScenes).toEqual([]);
  });

  test("a fatal render error supersedes local validation without stranding loading", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const encoded = readFileSync(LOCAL_SPLAT).toString("base64");
    const result = await page.evaluate(async (base64) => {
      const bytes = Uint8Array.from(atob(base64), (value) => value.charCodeAt(0));
      const pending = window.__viewer.openFile(new File([bytes], "pending.splat"));
      document.querySelector("canvas").dispatchEvent(
        new Event("webglcontextlost", { cancelable: true }),
      );
      await pending;
      const second = await window.__viewer.openFile(new File([bytes], "after-loss.splat"));
      return { second, stats: window.__viewer.getStats() };
    }, encoded);
    expect(result.second).toBeNull();
    expect(result.stats.loading).toBe(false);
    expect(result.stats.error).toBe("WebGL context lost");
    await expect(page.getByRole("alertdialog")).toContainText("WebGL context was lost");
    await expect(page.getByRole("button", { name: "Reload" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeHidden();
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

  test("opens a valid one-section KSPLAT file", async ({ page }) => {
    const errors = await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    await page.locator("#localFileInput").setInputFiles({
      name: "one-splat.ksplat",
      mimeType: "application/octet-stream",
      buffer: oneSectionKsplat(),
    });
    const stats = await page.evaluate(() => window.__viewer.waitRendered(30_000));
    expect(stats).toMatchObject({
      format: "KSPLAT",
      scene: "local-file",
      source: "local",
      splatCount: 1,
    });
    expect(errors, "KSPLAT import has no runtime errors").toEqual([]);
  });

  test("opens valid compressed PLY and SPZ LOD containers", async ({ page }) => {
    const errors = await boot(page, "wave-static");
    const identityPackedRotation = (0xc0000000 | (512 << 20) | (512 << 10) | 512) >>> 0;
    for (const [name, buffer, format] of [
      ["compressed.ply", compressedPly(identityPackedRotation), "PLY"],
      [
        "lod-leaf.spz",
        oneSplatSpz(3, [0, 0, 0, 0], { flags: 0x80, lodCount: 0, lodStart: 0 }),
        "SPZ",
      ],
    ]) {
      await page.locator("#localFileInput").setInputFiles({
        name,
        mimeType: "application/octet-stream",
        buffer,
      });
      await page.waitForFunction(() => window.__viewer.state.loading === false);
      const stats = await page.evaluate(() => window.__viewer.getStats());
      expect(stats.inputError, name).toBeNull();
      expect(stats.format, name).toBe(format);
    }
    expect(errors, "valid compressed containers have no runtime errors").toEqual([]);
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
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alertdialog");
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

  test("rejects Melkor canonical PLY before Spark decodes it", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const before = await page.evaluate(() => window.__viewer.getStats().scene);

    for (const [name, buffer] of [
      ["marked-canonical.ply", canonicalPly(true)],
      ["unmarked-canonical.ply", canonicalPly(false)],
    ]) {
      await page.locator("#localFileInput").setInputFiles({
        name, mimeType: "application/octet-stream", buffer,
      });
      await expect(page.getByRole("alertdialog"))
        .toContainText("Convert it to ply:graphdeco-3dgs-v1");
      expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
      await page.getByRole("button", { name: "Dismiss" }).click();
    }
  });

  test("rejects invalid local numeric payloads before Spark packs them", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const before = await page.evaluate(() => window.__viewer.getStats().scene);
    const markedBasicHeader = Buffer.from(
      "ply\nformat binary_little_endian 1.0\n" +
      "comment melkor_profile ply:graphdeco-3dgs-v1\n" +
      "element vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n",
    );
    const markedBasic = Buffer.concat([markedBasicHeader, Buffer.alloc(12)]);
    const cases = [
      ["wrong-profile.ply", markedBasic, "does not match its Graphdeco profile marker"],
      ["nan-position.ply", graphdecoPly({ position: [Number.NaN, 0, 0] }), "non-finite x value"],
      ["large-scale.ply", graphdecoPly({ scale: [10, 0, 0] }), "invalid scale"],
      ["zero-rotation.ply", graphdecoPly({ rotation: [0, 0, 0, 0] }), "invalid rotation"],
      [
        "overflowing-color.ply",
        graphdecoDoubleColorPly([Number.MAX_VALUE, 0, 0]),
        "f_dc_0 value outside the viewer range",
      ],
      ["invalid-packed-rotation.ply", compressedPly(0x3fffffff), "invalid packed rotation"],
      [
        "ambiguous-color.ply",
        graphdecoPlyWithExtraProperties(["red", "green", "blue"]),
        "multiple color property groups",
      ],
      [
        "ambiguous-opacity.ply",
        graphdecoPlyWithExtraProperties(["alpha"]),
        "multiple opacity properties",
      ],
      ["nan-position.splat", antiSplat({ position: [Number.NaN, 0, 0] }), "invalid position"],
      ["negative-scale.splat", antiSplat({ scale: [-1, 1, 1] }), "invalid scale"],
      ["large-scale.splat", antiSplat({ scale: [Math.exp(10), 1, 1] }), "invalid scale"],
      [
        "infinite-position.spz",
        oneSplatSpz(1, [128, 128, 128], { position: [0, 0x7c] }),
        "position 0 is not finite",
      ],
      [
        "large-position.spz",
        oneSplatSpz(2, [128, 128, 128], {
          fractionalBits: 0,
          position: [0xff, 0xff, 0x7f],
        }),
        "position 0 exceeds the viewer range",
      ],
      [
        "invalid-lod.spz",
        oneSplatSpz(3, [0, 0, 0, 0], { flags: 0x80, lodCount: 1, lodStart: 1 }),
        "LOD node 0 has an invalid child range",
      ],
      [
        "nan-position.ksplat",
        oneSectionKsplat({ position: [Number.NaN, 0, 0] }),
        "KSPLAT splat 0 has an invalid position",
      ],
      [
        "negative-scale.ksplat",
        oneSectionKsplat({ scale: [-1, 1, 1] }),
        "KSPLAT splat 0 has an invalid scale",
      ],
      [
        "large-scale.ksplat",
        oneSectionKsplat({ scale: [Math.exp(10), 1, 1] }),
        "KSPLAT splat 0 has an invalid scale",
      ],
      [
        "zero-rotation.ksplat",
        oneSectionKsplat({ rotation: [0, 0, 0, 0] }),
        "KSPLAT splat 0 has an invalid rotation",
      ],
      [
        "nan-sh.ksplat",
        oneSectionKsplat({ sh: [Number.NaN, 0, 0, 0, 0, 0, 0, 0, 0] }),
        "KSPLAT splat 0 has invalid SH data",
      ],
      [
        "nan-bucket.ksplat",
        oneSectionKsplat({
          compression: 1,
          position: [Number.NaN, 0, 0],
          rotation: [0x3c00, 0, 0, 0],
          scale: [0x3c00, 0x3c00, 0x3c00],
        }),
        "KSPLAT bucket 0 has an invalid center",
      ],
      [
        "infinite-half-scale.ksplat",
        oneSectionKsplat({
          compression: 1,
          rotation: [0x3c00, 0, 0, 0],
          scale: [0x7c00, 0x3c00, 0x3c00],
        }),
        "KSPLAT splat 0 has an invalid scale",
      ],
    ];
    for (const [name, buffer, message] of cases) {
      await page.locator("#localFileInput").setInputFiles({
        name, mimeType: "application/octet-stream", buffer,
      });
      await expect(page.getByRole("alertdialog")).toContainText(message);
      expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
      await page.getByRole("button", { name: "Dismiss" }).click();
    }
  });

  test("archive limits and local headers are checked before decoding", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    const before = await page.evaluate(() => window.__viewer.getStats().scene);
    await page.locator("#localFileInput").setInputFiles({
      name: "bomb.sog",
      mimeType: "application/zip",
      buffer: zipWithDeclaredSizes(1, 101),
    });
    await expect(page.getByRole("alertdialog")).toContainText("expansion ratio exceeds 100:1");
    expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
    await page.getByRole("button", { name: "Dismiss" }).click();

    const hiddenExpansion = deflatedZipWithExpandedSize(Buffer.alloc(1024 * 1024), 1);
    await page.locator("#localFileInput").setInputFiles({
      name: "hidden-expansion.sog",
      mimeType: "application/zip",
      buffer: hiddenExpansion,
    });
    await expect(page.getByRole("alertdialog"))
      .toContainText("expands beyond its directory size");
    expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
    await page.getByRole("button", { name: "Dismiss" }).click();

    const mismatched = zipWithDeclaredSizes(1, 1);
    mismatched.writeUInt16LE(0, 8);
    await page.locator("#localFileInput").setInputFiles({
      name: "mismatched.sog",
      mimeType: "application/zip",
      buffer: mismatched,
    });
    await expect(page.getByRole("alertdialog"))
      .toContainText("inconsistent local entry metadata");
    expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
    await page.getByRole("button", { name: "Dismiss" }).click();

    const metadata = {
      means: {
        shape: [1, 3], dtype: "float32", mins: [0, 0, 0], maxs: [1, 1, 1],
        files: ["means-low.png", "means-high.png"],
      },
      scales: {
        shape: [1, 3], dtype: "float32", mins: [0, 0, 0], maxs: [1, 1, 1],
        files: ["scales.png"],
      },
      quats: {
        shape: [1, 4], dtype: "uint8", encoding: "quaternion_packed",
        files: ["quats.png"],
      },
      sh0: {
        shape: [1, 1, 4], dtype: "float32", mins: [0, 0, 0, 0], maxs: [1, 1, 1, 1],
        files: ["sh0.png"],
      },
    };
    const oversizedImage = pngHeader(20_000, 20_000);
    const imageBomb = storedZip([
      ["meta.json", Buffer.from(JSON.stringify(metadata))],
      ["means-low.png", oversizedImage],
      ["means-high.png", oversizedImage],
      ["scales.png", oversizedImage],
      ["quats.png", oversizedImage],
      ["sh0.png", oversizedImage],
    ]);
    await page.locator("#localFileInput").setInputFiles({
      name: "image-bomb.sog",
      mimeType: "application/zip",
      buffer: imageBomb,
    });
    await expect(page.getByRole("alertdialog")).toContainText("local pixel limit");
    expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
    await page.getByRole("button", { name: "Dismiss" }).click();

    for (const [name, json] of [
      ["duplicate-names.sog", '{"means":{},"means":{}}'],
      ["non-finite-number.sog", '{"value":1e400}'],
    ]) {
      await page.locator("#localFileInput").setInputFiles({
        name,
        mimeType: "application/zip",
        buffer: storedZip([["meta.json", Buffer.from(json)]]),
      });
      await expect(page.getByRole("alertdialog")).toContainText("valid strict JSON");
      await page.getByRole("button", { name: "Dismiss" }).click();
    }

    const overflowingMeans = structuredClone(metadata);
    overflowingMeans.means.maxs = [100, 1, 1];
    await page.locator("#localFileInput").setInputFiles({
      name: "overflowing-means.sog",
      mimeType: "application/zip",
      buffer: storedZip([["meta.json", Buffer.from(JSON.stringify(overflowingMeans))]]),
    });
    await expect(page.getByRole("alertdialog")).toContainText("means range is invalid");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const overflowingScale = structuredClone(metadata);
    overflowingScale.scales.maxs = [10, 1, 1];
    await page.locator("#localFileInput").setInputFiles({
      name: "overflowing-scale.sog",
      mimeType: "application/zip",
      buffer: storedZip([["meta.json", Buffer.from(JSON.stringify(overflowingScale))]]),
    });
    await expect(page.getByRole("alertdialog")).toContainText("SOG v1 field layout is invalid");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const shMetadata = structuredClone(metadata);
    shMetadata.shN = {
      files: ["centroids.png", "labels.png"],
      maxs: 1,
      mins: -1,
      shape: [1, 9],
    };
    const onePixelImage = pngHeader(1, 1);
    await page.locator("#localFileInput").setInputFiles({
      name: "small-centroids.sog",
      mimeType: "application/zip",
      buffer: storedZip([
        ["meta.json", Buffer.from(JSON.stringify(shMetadata))],
        ["means-low.png", onePixelImage],
        ["means-high.png", onePixelImage],
        ["scales.png", onePixelImage],
        ["quats.png", onePixelImage],
        ["sh0.png", onePixelImage],
        ["centroids.png", onePixelImage],
        ["labels.png", onePixelImage],
      ]),
    });
    await expect(page.getByRole("alertdialog")).toContainText("at least 960 by 1024 pixels");
    expect(await page.evaluate(() => window.__viewer.getStats().scene)).toBe(before);
    await page.getByRole("button", { name: "Dismiss" }).click();
  });

  test("local container counts are checked before decoding", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));

    const ply = Buffer.from(
      "ply\nformat binary_little_endian 1.0\nelement vertex 5000001\n" +
      "property float x\nproperty float y\nproperty float z\nend_header\n",
    );
    await page.locator("#localFileInput").setInputFiles({
      name: "count-bomb.ply", mimeType: "application/octet-stream", buffer: ply,
    });
    await expect(page.getByRole("alertdialog")).toContainText("5,000,000 splats");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const loopingPly = Buffer.from(
      "ply\nformat binary_little_endian 1.0\nelement vertex 1\n" +
      "property float x\nproperty float y\nproperty float z\n" +
      "element ignored 5000000\nproperty uchar value\nend_header\n" +
      "\0".repeat(5_000_012), "binary",
    );
    await page.locator("#localFileInput").setInputFiles({
      name: "looping-elements.ply", mimeType: "application/octet-stream", buffer: loopingPly,
    });
    await expect(page.getByRole("alertdialog")).toContainText("unsupported element layout");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const truncatedPly = Buffer.concat([
      Buffer.from(
        "ply\nformat binary_little_endian 1.0\nelement vertex 1\n" +
        "property float x\nproperty float y\nproperty float z\nend_header\n",
      ),
      Buffer.alloc(4),
    ]);
    await page.locator("#localFileInput").setInputFiles({
      name: "truncated.ply", mimeType: "application/octet-stream", buffer: truncatedPly,
    });
    await expect(page.getByRole("alertdialog")).toContainText("size does not match");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const spzHeader = Buffer.alloc(16);
    spzHeader.writeUInt32LE(0x5053474e, 0);
    spzHeader.writeUInt32LE(3, 4);
    spzHeader.writeUInt32LE(5_000_001, 8);
    spzHeader.writeUInt8(12, 13);
    await page.locator("#localFileInput").setInputFiles({
      name: "count-bomb.spz",
      mimeType: "application/octet-stream",
      buffer: gzipSync(spzHeader),
    });
    await expect(page.getByRole("alertdialog")).toContainText("5,000,000 splats");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const ratioBombSpz = Buffer.alloc(16 + 1_000 * 20);
    ratioBombSpz.writeUInt32LE(0x5053474e, 0);
    ratioBombSpz.writeUInt32LE(3, 4);
    ratioBombSpz.writeUInt32LE(1_000, 8);
    ratioBombSpz.writeUInt8(12, 13);
    await page.locator("#localFileInput").setInputFiles({
      name: "ratio-bomb.spz",
      mimeType: "application/octet-stream",
      buffer: gzipSync(ratioBombSpz),
    });
    await expect(page.getByRole("alertdialog")).toContainText("SPZ expansion ratio exceeds 100:1");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const overlongSpz = Buffer.alloc(37);
    overlongSpz.writeUInt32LE(0x5053474e, 0);
    overlongSpz.writeUInt32LE(3, 4);
    overlongSpz.writeUInt32LE(1, 8);
    overlongSpz.writeUInt8(12, 13);
    await page.locator("#localFileInput").setInputFiles({
      name: "overlong.spz",
      mimeType: "application/octet-stream",
      buffer: gzipSync(overlongSpz),
    });
    await expect(page.getByRole("alertdialog")).toContainText("beyond its declared splats");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const shortSpz = overlongSpz.subarray(0, 35);
    await page.locator("#localFileInput").setInputFiles({
      name: "short.spz",
      mimeType: "application/octet-stream",
      buffer: gzipSync(shortSpz),
    });
    await expect(page.getByRole("alertdialog")).toContainText("shorter than its declared splats");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const ksplat = Buffer.alloc(4096);
    ksplat.writeUInt8(1, 1);
    ksplat.writeUInt32LE(4097, 4);
    await page.locator("#localFileInput").setInputFiles({
      name: "section-bomb.ksplat", mimeType: "application/octet-stream", buffer: ksplat,
    });
    await expect(page.getByRole("alertdialog")).toContainText("unsupported header values");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const mismatchedKsplat = oneSectionKsplat({ declaredCount: 2 });
    await page.locator("#localFileInput").setInputFiles({
      name: "mismatched-count.ksplat",
      mimeType: "application/octet-stream",
      buffer: mismatchedKsplat,
    });
    await expect(page.getByRole("alertdialog")).toContainText("inconsistent splat counts");
    await page.getByRole("button", { name: "Dismiss" }).click();

    const multiSectionKsplat = Buffer.alloc(4096 + 2 * 1024);
    multiSectionKsplat.writeUInt8(1, 1);
    multiSectionKsplat.writeUInt32LE(2, 4);
    multiSectionKsplat.writeUInt32LE(1, 16);
    await page.locator("#localFileInput").setInputFiles({
      name: "multiple-sections.ksplat",
      mimeType: "application/octet-stream",
      buffer: multiSectionKsplat,
    });
    await expect(page.getByRole("alertdialog")).toContainText("one KSPLAT section");
  });

  test("SPZ rotation data is checked before Spark decodes it", async ({ page }) => {
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));

    for (const [name, buffer] of [
      ["valid-v1.spz", oneSplatSpz(1, [216, 98, 215])],
      ["valid-v2.spz", oneSplatSpz(2, [128, 128, 128])],
      ["valid-v3.spz", oneSplatSpz(3, [0, 0, 0, 0])],
    ]) {
      await page.locator("#localFileInput").setInputFiles({
        name,
        mimeType: "application/octet-stream",
        buffer,
      });
      await page.waitForFunction(() => window.__viewer.state.loading === false);
      expect(await page.evaluate(() => window.__viewer.getStats().inputError), name).toBeNull();
    }

    await page.locator("#localFileInput").setInputFiles({
      name: "invalid-v1.spz",
      mimeType: "application/octet-stream",
      buffer: oneSplatSpz(1, [0, 0, 0]),
    });
    await expect(page.getByRole("alertdialog")).toContainText("quantization bound");
    await page.getByRole("button", { name: "Dismiss" }).click();

    await page.locator("#localFileInput").setInputFiles({
      name: "invalid-v3.spz",
      mimeType: "application/octet-stream",
      buffer: oneSplatSpz(3, [0xff, 0xff, 0xff, 0x3f]),
    });
    await expect(page.getByRole("alertdialog")).toContainText("invalid smallest-three data");
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
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alertdialog");
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
    const visible = await settledPixelStats(page);
    expect(visible.nonBgFrac, "last good local scene stays visible")
      .toBeGreaterThan(0.001);
    expect(visible.lumStd, "last good local scene is not a uniform frame").toBeGreaterThan(0.5);
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
    await page.getByRole("button", { name: "Reframe" }).click();
    expect(await page.locator("#orbitBtn").getAttribute("aria-pressed")).toBe("false");
    const reframed = await page.evaluate(() => window.__viewer.getStats().camera);
    await page.waitForTimeout(700);
    const settled = await page.evaluate(() => window.__viewer.getStats().camera);
    expect(Math.hypot(
      reframed[0] - settled[0], reframed[1] - settled[1], reframed[2] - settled[2],
    )).toBeLessThan(0.001);
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

  test("4D buffering exposes a distinct range state", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    await page.route("**/4d/wave/time_00001.ply", async (route) => {
      await new Promise((resolve) => setTimeout(resolve, 2_000));
      await route.continue();
    });
    await boot(page, "wave-static");
    await page.evaluate(() => { void window.__viewer.load("wave-4d"); });
    await page.waitForFunction(() => window.__viewer.get4DState().buffering === true, undefined,
      { timeout: 30_000 });

    await expect(page.locator("#tl")).toHaveAttribute("data-state", "buffering");
    await expect(page.locator("#tl")).toHaveAttribute("aria-busy", "true");
    await expect(page.locator("#tlStatus")).toContainText("Buffering frame 2");
    await expect(page.locator("#tlSlider")).toHaveAttribute("aria-valuetext", /Frame 1 of \d+/);

    await page.waitForFunction(() => window.__viewer.get4DState().buffering === false, undefined,
      { timeout: 30_000 });
    await expect(page.locator("#tl")).toHaveAttribute("aria-busy", "false");
    await expect(page.locator("#tlStatus")).toHaveText("Playing");
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
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alertdialog");
    await expect(page.getByRole("button", { name: "Retry" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Dismiss" })).toBeVisible();
    const retained = await page.evaluate(() => ({
      stats: window.__viewer.getStats(),
      sequence: window.__viewer.get4DState(),
    }));
    expect(retained.stats.scene).toBe(before.scene);
    expect(retained.sequence.count).toBe(0);
    const visible = await settledPixelStats(page);
    expect(visible.nonBgFrac, "active static scene remains visible")
      .toBeGreaterThan(0.001);
    expect(visible.lumStd, "active static scene is not a uniform frame").toBeGreaterThan(0.5);

    await page.getByRole("button", { name: "Dismiss" }).click();
    await page.waitForFunction(() => window.__viewer.getStats().rendered === true);
    expect((await page.evaluate(() => window.__viewer.getStats())).error).toBeNull();
  });

  test("4D manifests reject unsafe frame paths", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.waitRendered(30_000));
    await page.route("**/4d/wave/manifest.json", (route) => {
      if (route.request().method() === "GET") {
        return route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ fps: 12, frames: ["%2e%2e/secret.ply"] }),
        });
      }
      return route.continue();
    });
    await page.evaluate(() => window.__viewer.load("wave-4d").catch(() => {}));
    await expect(page.getByRole("alertdialog")).toContainText("Invalid 4D manifest");
    expect((await page.evaluate(() => window.__viewer.getStats())).scene).toBe("wave-static");
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
    await expect(page.locator("#overlay")).toHaveAttribute("role", "alertdialog");
    await expect(page.locator("#ov-title")).toContainText("PLY file has no complete header");
    const retained = await page.evaluate(() => ({
      stats: window.__viewer.getStats(),
      sequence: window.__viewer.get4DState(),
    }));
    expect(retained.stats.scene).toBe(before.scene);
    expect(retained.sequence.count).toBe(before.sequence.count);
    expect(retained.sequence.buffered).toBeGreaterThan(0);
    const visible = await settledPixelStats(page);
    expect(visible.nonBgFrac, "active 4D scene remains visible")
      .toBeGreaterThan(0.001);
    expect(visible.lumStd, "active 4D scene is not a uniform frame").toBeGreaterThan(0.5);

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

  test("rapid 4D seeks keep frame loads within the concurrency limit", async ({ page, request }) => {
    const hasManifest = (await request.fetch("/public/splats/4d/wave/manifest.json",
      { method: "HEAD" })).status() === 200;
    test.skip(!hasManifest, "4D demo sequence not generated (run node make-4d-demo.js)");

    await page.route("**/4d/wave/time_*.ply", async (route) => {
      await new Promise((resolve) => setTimeout(resolve, 250));
      await route.continue();
    });
    await boot(page, "wave-static");
    await page.evaluate(() => window.__viewer.load("wave-4d"));
    const observed = await page.evaluate(async () => {
      let peak = 0;
      for (let frame = 0; frame < 24; frame++) {
        void window.__viewer.seek4D((frame * 7) % 24);
        const state = window.__viewer.get4DState();
        peak = Math.max(peak, state.inFlight);
        await new Promise((resolve) => setTimeout(resolve, 10));
      }
      const state = window.__viewer.get4DState();
      return { peak, limit: state.maxInFlight };
    });
    expect(observed.peak).toBeGreaterThan(0);
    expect(observed.peak).toBeLessThanOrEqual(observed.limit);
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
    await expect(page.getByRole("button", { name: "Keep current frame" })).toBeVisible();

    await page.getByRole("button", { name: "Retry" }).click();
    await page.waitForFunction(() => window.__viewer.get4DState().error !== null, undefined,
      { timeout: 8_000 });
    expect(requests, "a failed explicit retry runs one new bounded retry cycle").toBe(6);
    expect(await page.getByRole("button", { name: "Retry" }).isVisible(),
      "a repeated failure remains recoverable").toBe(true);
    await page.getByRole("button", { name: "Keep current frame" }).click();
    await expect(page.getByRole("alertdialog")).toBeHidden();
    const recovered = await page.evaluate(() => ({
      sequence: window.__viewer.get4DState(),
      stats: window.__viewer.getStats(),
    }));
    expect(recovered.sequence.playing).toBe(false);
    expect(recovered.sequence.error).toBeNull();
    expect(recovered.stats.error).toBeNull();

    await page.evaluate(() => window.__viewer.load("wave-static"));
    expect((await page.evaluate(() => window.__viewer.waitRendered(30_000))).scene)
      .toBe("wave-static");
  });

  // The "4D format producer" packs each frame into SPZ and creates a manifest.
  // The temporal player streams it like the PLY sequence.
  // Skip this test if the SPZ pack is not generated.
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
