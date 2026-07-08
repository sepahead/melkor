#!/bin/bash

# Setup for STREAMING / online 3D Gaussian Splatting reconstruction.
#
# These build the splat scene *incrementally* — from a live/streamed video or
# an RGB-D sequence (SLAM), or per-frame for free-viewpoint video — rather
# than the offline SfM + training path. Their output is a standard INRIA 3DGS
# PLY, which melkor consumes directly (view it, or `melkor scene.ply
# scene.spz`).
#
# REALITY CHECK (verified against each repo's README):
#   * All are Linux + NVIDIA CUDA only (custom CUDA rasterizers, tiny-cuda-nn,
#     lietorch). None runs on macOS/Metal.
#   * None takes a plain folder of images. Each needs a CALIBRATED dataset in
#     a specific SLAM/video format (Replica, TUM-RGBD, ScanNet(++), or the
#     N3DV multi-view video rig) selected via a per-scene config. Running on
#     your own footage means authoring a config with correct intrinsics.
#   * Their environments are tool-specific (conda + CUDA submodule builds) and
#     cannot be installed generically, so this script CLONES and scaffolds
#     each repo, prints its license and the exact verified run command, and
#     defers the heavy CUDA/conda env to the repo's own README. It does not
#     fake a one-command install.
#
# LICENSING (melkor is MIT):
#   permissive  : Gaussian-SLAM (MIT), SplaTAM (BSD-3), Splat-SLAM (Apache-2.0)
#   noncommercial: 3DGStream (top-level MIT but inherits Inria Gaussian-
#                  Splatting's NON-COMMERCIAL license via its rasterizer
#                  submodule), MonoGS (Imperial College non-commercial)
# Non-permissive tools are refused unless you pass --accept-noncommercial.

set -e

log()  { echo "$@" >&2; }
warn() { echo "[WARN] $@" >&2; }
err()  { echo "[ERROR] $@" >&2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_ROOT/tools/streaming"

ACCEPT_NC=0
SELECTION=""

usage() {
    cat >&2 <<EOF
Usage: $0 <tool|group> [--accept-noncommercial]

All Linux+CUDA; need a calibrated SLAM/video dataset or config, not raw images.

Online reconstruction (SLAM):
  gaussian-slam   RGB-D dense SLAM -> standard 3DGS PLY directly     (MIT)
  splatam         RGB-D track & map -> PLY via export_ply.py         (BSD-3)
  splat-slam      RGB-only global SLAM -> PLY (non-default save)      (Apache, archived)
  3dgstream       Per-frame free-viewpoint video 3DGS                (non-commercial*)
  monogs          Monocular/RGB-D Gaussian SLAM                      (non-commercial)

4D / volumetric video (train -> per-frame PLY -> pack-4d.js -> viewer):
  4d-gs           4D-GS; export_perframe_3DGS.py -> time_*.ply       (non-commercial*)
  videogs         V3; per-frame point_cloud.ply checkpoints          (non-commercial*)
  gifstream       Custom compressed stream, no PLY export            (Apache, no PLY)
  spacetime       Spacetime-GS; non-standard PLY (not drop-in)       (non-commercial)
  ex4dgs          Ex4DGS; custom checkpoint, no PLY export           (CC-BY-NC)

Groups:
  permissive   all permissively-licensed tools (gaussian-slam, splatam,
               splat-slam; gifstream too, but it emits no standard PLY)
  slam         all SLAM / online-reconstruction tools
  4d           all 4D / volumetric-video tools
  list         print the catalog and exit
  all          everything (requires --accept-noncommercial)

* Several tools are permissive at the top level but their CUDA rasterizer /
  submodules carry the Inria Gaussian-Splatting NON-COMMERCIAL license
  (3dgstream, 4d-gs, videogs), so end-to-end use is research-only.
  4d-gs is the clean producer: its export feeds viewer/pack-4d.js directly.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --accept-noncommercial) ACCEPT_NC=1 ;;
        -h|--help)              usage; exit 0 ;;
        --*)                    err "Unknown flag: $arg"; usage; exit 1 ;;
        *)                      SELECTION="$arg" ;;
    esac
done
[ -z "$SELECTION" ] && { usage; exit 1; }

# name|repo|category|entry|dataset|ply|license_class|note
# category: slam (online reconstruction) | 4d (dynamic/volumetric video)
# NOTE: fields are '|'-separated, so no field may contain a literal '|'
# (use {a,b} not <a|b> in command templates).
catalog() {
    cat <<'EOF'
gaussian-slam|https://github.com/VladimirYugay/Gaussian-SLAM|slam|python run_slam.py configs/<ds>/<scene>.yaml --input_path <scene> --output_path <out>|Replica/TUM/ScanNet(++), RGB-D|direct|permissive|MIT; save_ply() writes canonical INRIA 3DGS PLY
splatam|https://github.com/spla-tam/SplaTAM|slam|python scripts/splatam.py configs/<ds>/splatam.py  (then python scripts/export_ply.py <config>)|Replica/TUM/ScanNet(++), RGB-D|export|permissive|BSD-3; PLY via scripts/export_ply.py
splat-slam|https://github.com/google-research/Splat-SLAM|slam|python run.py configs/<ds>/<scene>.yaml|Replica/TUM/ScanNet, RGB|non-default|permissive|Apache-2.0 (repo archived); save_gaussians() at src/utils/eval_utils.py
3dgstream|https://github.com/SJoJoK/3DGStream|slam|python train_frames.py --read_config --config_path <cfg.json> -m <frame0_3dgs> -v <scene> --image images_2|N3DV/Meet-Room multi-view video + COLMAP + frame-0 3DGS|frame0-only|noncommercial|Inria non-commercial via rasterizer submodule; per-frame = NTC deltas, not PLY
monogs|https://github.com/muskie82/MonoGS|slam|python slam.py --config configs/{mono,rgbd}/<ds>/<scene>.yaml|TUM/Replica/EuRoC|gui-only|noncommercial|Imperial College non-commercial; documents GUI/metrics, PLY export not documented
4d-gs|https://github.com/hustvl/4DGaussians|4d|python train.py -s data/<ds>/<scene> --configs arguments/<ds>/<scene>.py  (then python export_perframe_3DGS.py --iteration N --configs ... --model_path output/<ds>/<scene>)|D-NeRF (monocular) or DyNeRF/N3DV (multi-view + COLMAP)|perframe-ply|noncommercial|Apache top-level but Inria non-commercial rasterizer submodule; export_perframe_3DGS.py writes time_*.ply -> pack-4d.js
videogs|https://github.com/AuthorityWang/VideoGS|4d|python train_sequence.py --data datasets/<name> --output output/<name> --start 0 --end N|per-frame multi-view PNGs + transforms.json (ReRF/HiFi4G)|perframe-ckpt|noncommercial|V3; MIT top-level but Inria non-commercial submodules + NeuS2_K; per-frame point_cloud.ply checkpoints are packable
gifstream|https://github.com/XDimLab/GIFStream|4d|bash examples/benchmarks/multigop_gifstream.sh|multi-view video (GOP-based)|no-ply|permissive|Apache-2.0 (gsplat-fork rasterizer); custom compressed feature stream, NO standard PLY export
spacetime|https://github.com/oppo-us-research/SpacetimeGaussians|4d|python train.py --quiet --eval --config configs/n3d_lite/<scene>.json --model_path log/<scene> --source_path <path>|N3DV/Technicolor multi-view video|nonstd-ply|noncommercial|MIT top-level but "subject to Gaussian-Splatting use limitation"; non-standard PLY (trbf/motion), NOT drop-in
ex4dgs|https://github.com/juno181/Ex4DGS|4d|python train.py --config configs/<scene>.json --model_path <out> --source_path <path>|dynamic scene dataset|custom-ckpt|noncommercial|CC-BY-NC 4.0; custom checkpoint, no standard PLY export
EOF
}

if [ "$SELECTION" = "list" ]; then
    log "Streaming reconstruction + 4D volumetric video (see docs/STREAMING.md):"
    catalog | while IFS='|' read -r name repo cat entry ds ply lic note; do
        printf '  %-14s %-5s %-13s PLY:%-12s %s\n' "$name" "$cat" "$lic" "$ply" "$note" >&2
    done
    exit 0
fi

resolve() {
    case "$SELECTION" in
        permissive) catalog | awk -F'|' '$7=="permissive"{print $1}' ;;
        slam)       catalog | awk -F'|' '$3=="slam"{print $1}' ;;
        4d)         catalog | awk -F'|' '$3=="4d"{print $1}' ;;
        all)        catalog | awk -F'|' '{print $1}' ;;
        *)          catalog | awk -F'|' -v s="$SELECTION" '$1==s{print $1}' ;;
    esac
}
TARGETS="$(resolve)"
[ -z "$TARGETS" ] && { err "Unknown tool or group: $SELECTION"; usage; exit 1; }

command -v git >/dev/null || { err "git is required"; exit 1; }
if ! command -v nvidia-smi >/dev/null; then
    warn "nvidia-smi not found. These tools need an NVIDIA CUDA GPU to build and run."
fi
mkdir -p "$TOOLS_DIR"

setup_one() {
    local name="$1" repo="$2" cat="$3" entry="$4" ds="$5" ply="$6" lic="$7" note="$8"

    if [ "$lic" = "noncommercial" ] && [ "$ACCEPT_NC" -ne 1 ]; then
        warn "Skipping '$name': $note"
        warn "  Re-run with --accept-noncommercial if that fits your use."
        return 0
    fi

    local dir="$TOOLS_DIR/$name"
    log ""
    log "=== $name  [$lic] ==="
    if [ -d "$dir/.git" ]; then
        log "Updating $repo ..."
        git -C "$dir" pull --ff-only >&2 || warn "pull failed; keeping existing checkout"
        git -C "$dir" submodule update --init --recursive >&2 || true
    else
        log "Cloning $repo (recursive; CUDA submodules) ..."
        git clone --recursive "$repo" "$dir" >&2
    fi

    local lic_file
    lic_file="$(find "$dir" -maxdepth 1 -iname 'LICENSE*' | head -1)"
    [ -n "$lic_file" ] && log "License ($name): $(head -1 "$lic_file" | tr -d '\r')  [$lic_file]"

    # Pass-through wrapper (suffix by category). Does NOT invent a
    # folder-of-images interface: it forwards your args to the tool's real
    # entry point in its repo dir.
    local suffix="slam"; [ "$cat" = "4d" ] && suffix="4d"
    local wrapper="$PROJECT_ROOT/$name-$suffix"
    cat > "$wrapper" <<WRAP
#!/bin/bash
# Auto-generated by setup_streaming.sh — runs $name in its repo dir.
# Complete the tool's CUDA/conda env first (see tools/streaming/$name/README).
# Run command (verified from the repo README):
#   $entry
SCRIPT_DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
cd "\$SCRIPT_DIR/tools/streaming/$name"
exec "\$@"
WRAP
    chmod +x "$wrapper"

    log "Cloned -> $dir"
    log "Wrapper: $wrapper  (forwards args; complete the repo's CUDA env first)"
    log "Input : $ds  (a calibrated dataset/config, NOT a folder of images)"
    log "Run   : $entry"
    case "$ply" in
        direct)       log "Output: standard 3DGS PLY -> melkor scene.ply scene.spz" ;;
        export)       log "Output: run scripts/export_ply.py to get a 3DGS PLY -> melkor" ;;
        non-default)  log "Output: call its save_gaussians()/save_ply() to emit a 3DGS PLY -> melkor" ;;
        frame0-only)  log "Output: frame-0 is a standard 3DGS PLY (loadable); later frames are NTC deltas" ;;
        gui-only)     log "Output: GUI/metrics; PLY export is not documented upstream" ;;
        perframe-ply) log "Output: export_perframe_3DGS.py -> time_*.ply; then: node viewer/pack-4d.js <dir> --spz -> viewer" ;;
        perframe-ckpt)log "Output: per-frame point_cloud.ply checkpoints -> gather as time_*.ply -> pack-4d.js" ;;
        no-ply)       log "Output: custom compressed stream; NO standard PLY export (not for pack-4d.js)" ;;
        nonstd-ply)   log "Output: non-standard PLY (custom fields); NOT drop-in for pack-4d.js" ;;
        custom-ckpt)  log "Output: custom checkpoint; no standard PLY export" ;;
    esac
}

for t in $TARGETS; do
    line="$(catalog | awk -F'|' -v s="$t" '$1==s')"
    IFS='|' read -r name repo cat entry ds ply lic note <<< "$line"
    setup_one "$name" "$repo" "$cat" "$entry" "$ds" "$ply" "$lic" "$note"
done

log ""
log "Done. These are online/streaming 3DGS reconstruction pipelines; each needs"
log "its own CUDA/conda env (see each tools/streaming/<name>/README) and a"
log "calibrated dataset. Their 3DGS PLY output feeds melkor's viewer / SPZ."
log "See docs/STREAMING.md for the full picture."
