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

const port = Number(Bun.env.PORT ?? 8771);
const host = Bun.env.HOST ?? "127.0.0.1";
const root = import.meta.dir; // always serve the viewer/ directory
const log = Bun.env.QUIET ? () => {} : (...a) => console.log(...a);

Bun.serve({
  port,
  hostname: host,
  async fetch(req) {
    if (req.method !== "GET" && req.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405, headers: { allow: "GET, HEAD" } });
    }
    let pathname;
    try {
      pathname = decodeURIComponent(new URL(req.url).pathname);
    } catch {
      return new Response("Bad request", { status: 400 });
    }
    if (pathname.endsWith("/")) pathname += "index.html";
    // Resolve first, then apply the public-path allowlist to the canonical
    // relative path. Checking the URL prefix before resolution lets an encoded
    // slash turn `/vendor/..%2Fpackage.json` into a private in-root file.
    const filePath = resolve(root, "." + pathname);
    const rel = relative(root, filePath);
    if (isAbsolute(rel) || rel === ".." || rel.startsWith(".." + sep)) {
      log(req.method, pathname, 403);
      return new Response("Forbidden", { status: 403 });
    }
    const publicPath = rel.split(sep).join("/");
    if (publicPath !== "index.html" &&
        !publicPath.startsWith("vendor/") &&
        !publicPath.startsWith("public/")) {
      log(req.method, pathname, 404);
      return new Response("Not found", { status: 404 });
    }
    try {
      const file = Bun.file(filePath);
      if (!(await file.exists())) {
        log(req.method, pathname, 404);
        return new Response("Not found", { status: 404 });
      }
      log(req.method, pathname, 200);
      if (req.method === "HEAD") {
        return new Response(null, {
          status: 200,
          headers: { "content-type": file.type, "content-length": String(file.size) },
        });
      }
      return new Response(file); // BunFile Response handles Content-Type + Range
    } catch {
      return new Response("Internal server error", { status: 500 });
    }
  },
});

console.log(`Melkor viewer  →  http://${host === "0.0.0.0" ? "localhost" : host}:${port}/`);
