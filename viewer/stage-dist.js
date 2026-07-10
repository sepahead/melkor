#!/usr/bin/env bun
// Stage a deterministic frontend for `tauri build`. Third-party scene captures
// are developer/test fixtures only and are deliberately excluded; the desktop
// app embeds project-owned generated demos plus MIT-licensed runtime code.
import { cp, mkdir, readFile, rename, rm, stat } from "node:fs/promises";
import { createHash } from "node:crypto";
import { dirname, join } from "node:path";

const root = import.meta.dir;
const dist = join(root, "dist");
const staging = join(root, `.dist-stage-${process.pid}`);
const copies = [
  ["../LICENSE", "LICENSE"],
  ["index.html", "index.html"],
  ["THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md"],
  ["RUST_THIRD_PARTY_LICENSES.html", "RUST_THIRD_PARTY_LICENSES.html"],
  ["ASSET_PROVENANCE.md", "ASSET_PROVENANCE.md"],
  ["vendor/three.module.js", "vendor/three.module.js"],
  ["vendor/three.core.js", "vendor/three.core.js"],
  ["vendor/addons/postprocessing/Pass.js", "vendor/addons/postprocessing/Pass.js"],
  ["vendor/spark.module.js", "vendor/spark.module.js"],
  ["public/splats/generated/wave.splat", "public/splats/generated/wave.splat"],
  ["public/splats/4d/wave/manifest.json", "public/splats/4d/wave/manifest.json"],
];
const required = [
  "../LICENSE",
  "index.html",
  "THIRD_PARTY_NOTICES.md",
  "RUST_THIRD_PARTY_LICENSES.html",
  "ASSET_PROVENANCE.md",
  "vendor/three.module.js",
  "vendor/three.core.js",
  "vendor/addons/postprocessing/Pass.js",
  "vendor/spark.module.js",
  "public/splats/generated/wave.splat",
  "public/splats/4d/wave/manifest.json",
];
const expectedHashes = new Map([
  ["vendor/three.module.js", "c8211c69345d2e9949dc7a8ac969380497aa0600a5a8ac6a459c8cd02dd9cb8a"],
  ["vendor/three.core.js", "eb077d2417f61d3e6d9264c317cabc4ea35769ed6b0ab533067292a550784c20"],
  ["vendor/addons/postprocessing/Pass.js", "444b409c235ead986893c472e720da1b779a56985c7d10b279c7944b52bd61c5"],
  ["vendor/spark.module.js", "c0355a962f68a6de9b13df69f05b1aba3614d9aec43a4504975daeb349126a8a"],
  ["public/splats/generated/wave.splat", "dea9796bfe08f0a0ee58d0d8932ba8918db72a1721f6742af83b96a52894cb19"],
  ["public/splats/4d/wave/manifest.json", "b64750813d99b6db81d4927e770febef6455eacd4b5c064de14a6664f6d5ccce"],
]);

async function requireFile(relativePath) {
  const info = await stat(join(root, relativePath));
  if (!info.isFile() || info.size === 0) {
    throw new Error(`required viewer asset is empty: ${relativePath}`);
  }
}

for (const item of required) await requireFile(item);
for (const [relativePath, expected] of expectedHashes) {
  const bytes = await readFile(join(root, relativePath));
  const actual = createHash("sha256").update(bytes).digest("hex");
  if (actual !== expected) {
    throw new Error(`viewer runtime digest mismatch: ${relativePath}`);
  }
}

// Validate every generated frame referenced by the manifest. A malformed or
// partial sequence must fail the build rather than ship a viewer that stalls.
const manifestPath = "public/splats/4d/wave/manifest.json";
const manifest = JSON.parse(await readFile(join(root, manifestPath), "utf8"));
if (!Number.isFinite(manifest.fps) || manifest.fps <= 0 ||
    !Array.isArray(manifest.frames) || manifest.frames.length === 0) {
  throw new Error("generated 4D manifest must contain positive fps and frames");
}
for (const frame of manifest.frames) {
  if (typeof frame !== "string" || !/^[A-Za-z0-9._-]+\.ply$/.test(frame)) {
    throw new Error(`unsafe generated frame name: ${String(frame)}`);
  }
  await requireFile(`public/splats/4d/wave/${frame}`);
  copies.push([
    `public/splats/4d/wave/${frame}`,
    `public/splats/4d/wave/${frame}`,
  ]);
}

await rm(staging, { recursive: true, force: true });
for (const [source, target] of copies) {
  const destination = join(staging, target);
  await mkdir(dirname(destination), { recursive: true });
  await cp(join(root, source), destination);
  console.log(`staged ${source}`);
}
await rm(dist, { recursive: true, force: true });
await rename(staging, dist);
console.log(`dist/ ready at ${dist} (external scene fixtures excluded)`);
