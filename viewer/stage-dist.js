#!/usr/bin/env bun
// Stage a clean frontend bundle for `tauri build` (frontendDist = ../dist), so the
// app bundle embeds only what the webview needs — never node_modules/, src-tauri/
// target/, tests/, or screenshots/. Run automatically by `tauri build`
// (beforeBuildCommand); safe to run by hand.
import { rm, mkdir, cp, stat } from "node:fs/promises";
import { join } from "node:path";

const root = import.meta.dir;
const dist = join(root, "dist");
const items = ["index.html", "vendor", "public"]; // everything the importmap + loader needs

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });
for (const item of items) {
  try {
    await stat(join(root, item));
    await cp(join(root, item), join(dist, item), { recursive: true });
    console.log(`staged ${item}`);
  } catch {
    console.warn(`skip ${item} (missing — run ./fetch-assets.sh)`);
  }
}
console.log(`dist/ ready at ${dist}`);
