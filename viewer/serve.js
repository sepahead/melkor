#!/usr/bin/env bun
// Minimal static file server for the SparkJS viewer — Bun-native, zero deps.
// Plain static serving (no bundling) so the browser resolves the importmap and
// BunFile responses give correct MIME types + HTTP range support out of the box.
// Also serves as the Tauri dev server (see src-tauri/tauri.conf.json devUrl).
//
//   bun viewer/serve.js          # from repo root
//   bun run serve                # from viewer/  (package.json script)
//   PORT=3000 bun serve.js       # QUIET=1 to silence the request log
import { isAbsolute, relative, resolve, sep } from "node:path";
import { createHash } from "node:crypto";
import { readFile, realpath } from "node:fs/promises";

const portText = Bun.env.PORT ?? "8771";
if (!/^\d+$/.test(portText) || Number(portText) < 1 || Number(portText) > 65535) {
  console.error("error: PORT must be an integer from 1 through 65535");
  process.exit(2);
}
const port = Number(portText);
const host = Bun.env.HOST ?? "127.0.0.1";
const root = import.meta.dir; // always serve the viewer/ directory
const canonicalRoot = await realpath(root);
const log = Bun.env.QUIET ? () => {} : (...a) => console.log(...a);
const indexText = await readFile(resolve(root, "index.html"), "utf8");
const importMaps = [...indexText.matchAll(/<script type="importmap">([\s\S]*?)<\/script>/g)];
if (importMaps.length !== 1) {
  console.error("error: index.html must contain one inline import map");
  process.exit(2);
}
const importMapHash = createHash("sha256").update(importMaps[0][1]).digest("base64");
const securityHeaders = {
  "content-security-policy": [
    "default-src 'self'",
    "base-uri 'none'",
    "connect-src 'self' data:",
    "font-src 'self' data:",
    "form-action 'none'",
    "frame-ancestors 'none'",
    "img-src 'self' blob: data:",
    "manifest-src 'none'",
    "media-src 'none'",
    "object-src 'none'",
    `script-src 'self' 'sha256-${importMapHash}' 'wasm-unsafe-eval'`,
    "style-src 'self'",
    "worker-src 'self' blob: data:",
  ].join("; "),
  "permissions-policy": "camera=(), geolocation=(), microphone=(), payment=(), usb=()",
  "referrer-policy": "no-referrer",
  "x-content-type-options": "nosniff",
  "x-frame-options": "DENY",
};

function response(body, options = {}) {
  const headers = new Headers(securityHeaders);
  for (const [name, value] of new Headers(options.headers)) headers.set(name, value);
  return new Response(body, { ...options, headers });
}

function logRequest(method, pathname, status) {
  log(method, JSON.stringify(pathname), status);
}

function isPublicPath(relativePath) {
  const normalized = relativePath.split(sep).join("/");
  return normalized === "index.html" ||
    normalized === "favicon.png" ||
    normalized === "bootstrap.js" ||
    normalized === "local-file-validator.js" ||
    normalized === "local-file-validator-worker.js" ||
    normalized === "viewer.css" ||
    normalized === "viewer.js" ||
    normalized.startsWith("vendor/") ||
    normalized.startsWith("public/");
}

function sourcePath(relativePath) {
  const normalized = relativePath.split(sep).join("/");
  return normalized === "favicon.png" ? "src-tauri/icons/32x32.png" : normalized;
}

Bun.serve({
  port,
  hostname: host,
  async fetch(req) {
    if (req.method !== "GET" && req.method !== "HEAD") {
      return response("Method not allowed", { status: 405, headers: { allow: "GET, HEAD" } });
    }
    let pathname;
    try {
      pathname = decodeURIComponent(new URL(req.url).pathname);
    } catch {
      return response("Bad request", { status: 400 });
    }
    if (pathname.endsWith("/")) pathname += "index.html";
    // Resolve first, then apply the public-path allowlist to the canonical
    // relative path. Checking the URL prefix before resolution lets an encoded
    // slash turn `/vendor/..%2Fpackage.json` into a private in-root file.
    const requestPath = resolve(root, "." + pathname);
    const rel = relative(root, requestPath);
    if (isAbsolute(rel) || rel === ".." || rel.startsWith(".." + sep)) {
      logRequest(req.method, pathname, 403);
      return response("Forbidden", { status: 403 });
    }
    if (!isPublicPath(rel)) {
      logRequest(req.method, pathname, 404);
      return response("Not found", { status: 404 });
    }
    const expectedSource = sourcePath(rel);
    const filePath = resolve(root, expectedSource);
    try {
      const canonicalFile = await realpath(filePath);
      const canonicalRel = relative(canonicalRoot, canonicalFile);
      if (isAbsolute(canonicalRel) || canonicalRel === ".." ||
          canonicalRel.startsWith(".." + sep) ||
          canonicalRel.split(sep).join("/") !== expectedSource) {
        logRequest(req.method, pathname, 403);
        return response("Forbidden", { status: 403 });
      }
      const file = Bun.file(canonicalFile);
      logRequest(req.method, pathname, 200);
      if (req.method === "HEAD") {
        return response(null, {
          status: 200,
          headers: { "content-type": file.type, "content-length": String(file.size) },
        });
      }
      return response(file, { headers: { "content-type": file.type } });
    } catch (error) {
      if (error && typeof error === "object" && error.code === "ENOENT") {
        logRequest(req.method, pathname, 404);
        return response("Not found", { status: 404 });
      }
      return response("Internal server error", { status: 500 });
    }
  },
});

console.log(`Melkor viewer  →  http://${host === "0.0.0.0" ? "localhost" : host}:${port}/`);
