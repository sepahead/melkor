#!/usr/bin/env bash
# Download the SparkJS viewer's runtime dependencies and city/area splat scenes.
# These are gitignored (large / third-party); run this once after cloning.
#
#   ./viewer/fetch-assets.sh
#
# Idempotent + integrity-checked: every artifact is verified against a pinned
# SHA-256 on each run (both freshly fetched and already-present files). A
# mismatch aborts the script instead of shipping untrusted bytes. To bump a
# version on purpose, update its URL and the matching hash together.
set -euo pipefail

cd "$(dirname "$0")"

THREE_VER="0.180.0"
SPARK_VER="2.1.0"
JSDELIVR="https://cdn.jsdelivr.net/npm/three@${THREE_VER}"

mkdir -p vendor/addons/postprocessing public/splats

# path|url|sha256  — relative to vendor/ or public/splats/. The pinned SHA-256
# is the supply-chain control: the splat URLs and the Huggingface `main` ref
# are NOT version-pinned (mutable), and even the version-pinned JS is fetched
# from third-party CDNs and then EXECUTED in the browser. Verifying the digest
# turns a tampered/re-pointed/corrupted response into a hard failure.
vendor_files=(
  "vendor/three.module.js|${JSDELIVR}/build/three.module.js|c8211c69345d2e9949dc7a8ac969380497aa0600a5a8ac6a459c8cd02dd9cb8a"
  "vendor/three.core.js|${JSDELIVR}/build/three.core.js|eb077d2417f61d3e6d9264c317cabc4ea35769ed6b0ab533067292a550784c20"
  "vendor/addons/postprocessing/Pass.js|${JSDELIVR}/examples/jsm/postprocessing/Pass.js|444b409c235ead986893c472e720da1b779a56985c7d10b279c7944b52bd61c5"
  "vendor/spark.module.js|https://sparkjs.dev/releases/spark/${SPARK_VER}/spark.module.js|c0355a962f68a6de9b13df69f05b1aba3614d9aec43a4504975daeb349126a8a"
)

# City / area Gaussian-splat scenes (SPZ, SOG/.zip, .splat — every format Spark auto-loads).
splat_files=(
  "public/splats/snow-street.spz|https://sparkjs.dev/assets/splats/snow-street.spz|35840fd3d178388409096537e2f363c49e1502850c5bb81dbaa5cbc4ecb3ecaa"
  "public/splats/valley.spz|https://sparkjs.dev/assets/splats/valley.spz|565768b0b758698a641521654ac0947a74c85cb69ee0f034e6c928fbefa97833"
  "public/splats/distant-igloo.spz|https://sparkjs.dev/assets/splats/distant-igloo.spz|41579c3bfc754fd40da3db0c9924cfd8b70b403d7448e0f90935e8f62d6f5121"
  "public/splats/sutro.zip|https://sparkjs.dev/assets/splats/sutro.zip|6a76be688a4c8066be1de8a951f179bee2463d0fc9dbd6cf4389a15db6a161a9"
  "public/splats/train.splat|https://huggingface.co/cakewalk/splat-data/resolve/main/train.splat|d371f544e9e6a0a10b4a9d75c48d66f3c47f484559c33883c33c97ad7d2aa6af"
)

# SHA-256 helper, resolved once and fail-closed: prefer sha256sum (Linux/CI),
# fall back to shasum (macOS). If NEITHER exists we abort rather than silently
# skipping the integrity check — a skipped check is the same as no check.
if command -v sha256sum >/dev/null 2>&1; then _SHA="sha256sum"
elif command -v shasum >/dev/null 2>&1; then _SHA="shasum -a 256"
else echo "ERROR: need sha256sum or shasum to verify downloads; aborting" >&2; exit 1
fi
sha256() { $_SHA "$1" | cut -d' ' -f1; }

# Fetch with mandatory integrity check: download to a temp file, verify the
# pinned digest, then atomically move into place. A pre-existing file is
# re-verified and only re-fetched if it no longer matches.
fetch() {
  local entry="$1" path url want got tmp
  path="${entry%%|*}"; entry="${entry#*|}"; url="${entry%%|*}"; want="${entry##*|}"
  if [ -s "$path" ]; then
    got="$(sha256 "$path")"
    if [ "$got" = "$want" ]; then echo "ok     $path (verified)"; return; fi
    echo "warn   $path checksum drift, re-fetching"; rm -f "$path"
  fi
  echo "fetch  $path"
  tmp="${path}.part"
  curl -fSL --connect-timeout 20 --max-time 900 -o "$tmp" "$url"
  got="$(sha256 "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "ERROR  $path SHA-256 mismatch — refusing tampered/changed asset" >&2
    echo "         url:      $url" >&2
    echo "         expected: $want" >&2
    echo "         got:      $got" >&2
    rm -f "$tmp"
    exit 1
  fi
  mv "$tmp" "$path"
  echo "ok     $path (verified)"
}

echo "== runtime libraries =="
for f in "${vendor_files[@]}"; do fetch "$f"; done
echo "== splat scenes =="
for f in "${splat_files[@]}"; do fetch "$f"; done

# Optional: dogfood melkor to emit a standard 3DGS .ply from one scene, so the
# viewer's PLY entry (the 4th SparkJS format) lights up. Needs melkor built once:
#   (cd .. && mkdir -p build && cd build && cmake .. && make melkor -j)
MELKOR_BIN="$(ls ../build/melkor 2>/dev/null || true)"
PLY_OUT="public/splats/distant-igloo.ply"
if [ -n "$MELKOR_BIN" ] && [ -s "public/splats/distant-igloo.spz" ] && [ ! -s "$PLY_OUT" ]; then
  echo "== melkor SPZ->PLY =="
  echo "convert $PLY_OUT (via $MELKOR_BIN)"
  "$MELKOR_BIN" public/splats/distant-igloo.spz "$PLY_OUT" >/dev/null && echo "ok     $PLY_OUT"
elif [ ! -s "$PLY_OUT" ]; then
  echo "note: build melkor to generate $PLY_OUT and enable the PLY scene (optional)"
fi

# Optional: generate the synthetic 4D (temporal) demo sequence so the viewer's
# "Wave · 4D" scene and its temporal player light up. Real 4D content: run
# 4D-GS export_perframe_3DGS.py and drop time_*.ply + a manifest.json here.
if command -v node >/dev/null 2>&1 && [ ! -s "public/splats/4d/wave/manifest.json" ]; then
  echo "== 4D demo sequence =="
  node make-4d-demo.js && echo "ok     public/splats/4d/wave/ (24 frames)"
fi
# Optional: pack the demo to per-frame SPZ (the "4D format producer" path).
# Needs the melkor binary; demonstrates ~94% smaller streamable 4D.
if command -v node >/dev/null 2>&1 && [ -n "$MELKOR_BIN" ] \
   && [ -s "public/splats/4d/wave/manifest.json" ] \
   && [ ! -s "public/splats/4d/wave-spz/manifest.json" ]; then
  echo "== 4D SPZ pack =="
  node pack-4d.js public/splats/4d/wave --spz --out public/splats/4d/wave-spz --melkor "$MELKOR_BIN"
fi

echo "Done. Serve with:  python3 -m http.server 8771 --bind 127.0.0.1  (then open /index.html)"
