#!/bin/bash

# Setup for SOTA feedforward reconstruction models (late 2025 - 2026).
#
# These supplement Depth-Anything-3 (setup_da3.sh) with newer pose-free
# geometry models and direct feedforward Gaussian-splatting models. Two
# integration shapes:
#
#   * Geometry models (VGGT, MapAnything, Pi3, AMB3R) predict cameras +
#     point maps from uncalibrated images and can EXPORT a COLMAP `sparse/`
#     reconstruction, which drops straight into melkor's existing training
#     path (pipeline.sh / opensplat_wrapper.sh, both of which consume a
#     COLMAP project). No SfM (COLMAP/GLOMAP) run needed.
#   * Direct-splat models (YoNoSplat, SPFSplatV2) output 3D Gaussians in a
#     single forward pass, written to PLY for the viewer / further training.
#   * MoGe-2 is single-image geometry (per-image points/depth/normals).
#
# LICENSING (this matters — melkor itself is MIT):
#   permissive : MapAnything (code Apache-2.0, `-apache` weights Apache-2.0),
#                YoNoSplat (MIT), SPFSplatV2 (MIT), MoGe-2 (MIT)
#   non-commercial weights : Pi3 (code BSD-3, weights CC-BY-NC-4.0),
#                VGGT (weights CC-BY-NC-4.0; a gated commercial checkpoint
#                exists), MapAnything `facebook/map-anything` (CC-BY-NC-4.0)
#   unlicensed : AMB3R has NO LICENSE file at time of writing (all rights
#                reserved by default) — research/evaluation only until the
#                authors publish terms.
#
# Non-permissive models are refused unless you pass the matching flag,
# acknowledging you have verified the terms for your use:
#   --accept-noncommercial   allow CC-BY-NC weights (Pi3, VGGT, MapAnything-nc)
#   --accept-unlicensed      allow AMB3R (no license published)
#
# Each install re-reads and prints the cloned repo's actual LICENSE so the
# terms shown are always current, not what this script assumed.
#
# NOTE: these models require Linux + NVIDIA CUDA + a recent PyTorch and
# download multi-GB weights; they do not run on the macOS/Metal build.

set -e

# ---- logging (to stderr so any captured stdout stays clean) ---------------
log()  { echo "$@" >&2; }
warn() { echo "[WARN] $@" >&2; }
err()  { echo "[ERROR] $@" >&2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_ROOT/tools"

ACCEPT_NC=0
ACCEPT_UNLICENSED=0
SELECTION=""

usage() {
    cat >&2 <<EOF
Usage: $0 <model|group> [--accept-noncommercial] [--accept-unlicensed]

Models:
  vggt          Geometry + camera poses; cleanest COLMAP export      (weights NC)
  mapanything   Universal metric geometry (Apache weights available) (permissive*)
  pi3           Pose-free permutation-equivariant geometry           (weights NC)
  amb3r         Metric geometry + SLAM/SfM; reports beating DA3       (UNLICENSED)
  yonosplat     Pose-free feedforward 3DGS -> splats directly         (MIT)
  spfsplatv2    Self-supervised pose-free feedforward 3DGS            (MIT)
  moge2         Single-image geometry (points/depth/normals)          (MIT)

Groups:
  permissive    Install all permissively-licensed models
                (mapanything-apache, yonosplat, spfsplatv2, moge2)
  list          Print the catalog and exit
  all           Everything (requires both --accept-* flags)

* mapanything code is Apache-2.0; use the facebook/map-anything-apache weights
  for commercial use. The default facebook/map-anything weights are CC-BY-NC-4.0.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --accept-noncommercial) ACCEPT_NC=1 ;;
        --accept-unlicensed)    ACCEPT_UNLICENSED=1 ;;
        -h|--help)              usage; exit 0 ;;
        --*)                    err "Unknown flag: $arg"; usage; exit 1 ;;
        *)                      SELECTION="$arg" ;;
    esac
done

if [ -z "$SELECTION" ]; then usage; exit 1; fi

# ---- catalog: name|repo|category|license_class|note ------------------------
# license_class: permissive | noncommercial | unlicensed
catalog() {
    cat <<'EOF'
vggt|https://github.com/facebookresearch/vggt|geometry-colmap|noncommercial|Meta, CVPR'25; demo_colmap.py --scene_dir exports sparse/ for melkor training
mapanything|https://github.com/facebookresearch/map-anything|geometry-colmap|permissive|Meta+CMU, 3DV'26; scripts/demo_colmap.py; --apache flag = Apache weights
pi3|https://github.com/yyfz/Pi3|geometry-ply|noncommercial|ICLR'26; example_mm.py writes a PLY point cloud; code BSD-3, weights CC-BY-NC
amb3r|https://github.com/HengyiWang/amb3r|geometry-ply|unlicensed|UCL, CVPR'26; reports beating DA3; sfm/run.py; no LICENSE published
yonosplat|https://github.com/cvg/YoNoSplat|dataset-eval|permissive|ETH Zurich, ICLR'26; MIT; Hydra/dataset eval, NOT a folder-of-images tool
spfsplatv2|https://github.com/ranrhuang/SPFSplatV2|dataset-eval|permissive|Imperial College; MIT; Hydra/dataset eval, needs RE10K/ACID format
moge2|https://github.com/microsoft/MoGe|geometry-mono|permissive|Microsoft; MIT; `moge infer` single-image geometry
EOF
}

if [ "$SELECTION" = "list" ]; then
    log "SOTA feedforward models (see docs/FEEDFORWARD_SOTA.md):"
    catalog | while IFS='|' read -r name repo cat lic note; do
        printf '  %-12s %-14s %-14s %s\n' "$name" "$cat" "$lic" "$note" >&2
    done
    exit 0
fi

# Resolve selection -> list of catalog names.
resolve() {
    case "$SELECTION" in
        permissive) catalog | awk -F'|' '$4=="permissive"{print $1}' ;;
        all)        catalog | awk -F'|' '{print $1}' ;;
        *)          catalog | awk -F'|' -v s="$SELECTION" '$1==s{print $1}' ;;
    esac
}

TARGETS="$(resolve)"
if [ -z "$TARGETS" ]; then
    err "Unknown model or group: $SELECTION"
    usage
    exit 1
fi

# ---- preflight -------------------------------------------------------------
command -v git >/dev/null    || { err "git is required"; exit 1; }
command -v python3 >/dev/null || { err "python3 is required"; exit 1; }
if ! command -v nvidia-smi >/dev/null; then
    warn "nvidia-smi not found. These models require an NVIDIA GPU to run;"
    warn "cloning/installing will proceed but inference needs CUDA hardware."
fi

mkdir -p "$TOOLS_DIR"

install_one() {
    local name="$1" repo="$2" cat="$3" lic="$4" note="$5"

    # License gate.
    if [ "$lic" = "noncommercial" ] && [ "$ACCEPT_NC" -ne 1 ]; then
        warn "Skipping '$name': weights are non-commercial (CC-BY-NC-4.0)."
        warn "  Re-run with --accept-noncommercial if that fits your use."
        return 0
    fi
    if [ "$lic" = "unlicensed" ] && [ "$ACCEPT_UNLICENSED" -ne 1 ]; then
        warn "Skipping '$name': no LICENSE published (all rights reserved)."
        warn "  Re-run with --accept-unlicensed for research/evaluation only."
        return 0
    fi

    local dir="$TOOLS_DIR/$name"
    log ""
    log "=== $name  [$cat, $lic] ==="
    log "$note"

    if [ -d "$dir/repo/.git" ]; then
        log "Updating $repo ..."
        git -C "$dir/repo" pull --ff-only >&2 || warn "pull failed; keeping existing checkout"
    else
        mkdir -p "$dir"
        log "Cloning $repo ..."
        git clone --depth 1 "$repo" "$dir/repo" >&2
    fi

    # Surface the ACTUAL license from the checkout.
    local lic_file
    lic_file="$(find "$dir/repo" -maxdepth 1 -iname 'LICENSE*' | head -1)"
    if [ -n "$lic_file" ]; then
        log "License ($name): $(head -1 "$lic_file" | tr -d '\r')  [$lic_file]"
    else
        warn "$name: no LICENSE file in the repo — treat as all-rights-reserved."
    fi

    # Per-model venv + install. We defer to each repo's own install and
    # weight auto-download (from_pretrained / documented CLI) rather than
    # hardcoding fragile weight URLs.
    local venv="$dir/venv"
    if [ ! -d "$venv" ]; then
        python3 -m venv "$venv" >&2
    fi
    # shellcheck disable=SC1091
    source "$venv/bin/activate"
    pip install --upgrade pip >&2

    # Install steps verified against each repo. Torch/CUDA is intentionally
    # left to the user (must match their CUDA); repos that pin it in
    # requirements.txt handle it there.
    case "$name" in
        vggt)
            pip install -r "$dir/repo/requirements.txt" >&2 || pip install -e "$dir/repo" >&2 ;;
        mapanything|moge2)
            # Both install a package (mapanything: pip install -e .; moge:
            # provides the `moge` console script).
            pip install -e "$dir/repo" >&2 ;;
        pi3|yonosplat|spfsplatv2)
            if [ -f "$dir/repo/requirements.txt" ]; then
                pip install -r "$dir/repo/requirements.txt" >&2
            fi
            pip install -e "$dir/repo" >&2 || true ;;
        amb3r)
            if [ -f "$dir/repo/requirements.txt" ]; then
                pip install -r "$dir/repo/requirements.txt" >&2
            fi ;;
    esac
    deactivate || true

    create_wrapper "$name" "$cat"
    log "Installed '$name' -> $dir"
}

# Thin project-root wrapper that activates the model's venv and runs its
# canonical entry point. Mirrors the da3-infer pattern.
create_wrapper() {
    local name="$1" cat="$2"
    local wrapper="$PROJECT_ROOT/$name-infer"
    # Entry points verified against each repo's README/docs.
    local entry
    case "$name" in
        vggt)        entry='python demo_colmap.py "$@"' ;;          # --scene_dir=DIR (images in DIR/images/) -> DIR/sparse/
        mapanything) entry='python scripts/demo_colmap.py "$@"' ;;  # --images_dir=DIR --output_dir=OUT [--apache] -> COLMAP sparse/
        pi3)         entry='python example_mm.py "$@"' ;;           # --data_path DIR --save_path out.ply -> PLY point cloud
        amb3r)       entry='python sfm/run.py "$@"' ;;              # --data_path DIR (also ./demo.py, slam/run.py)
        moge2)       entry='moge infer "$@"' ;;                     # -i IMAGES -o OUT --ply --glb --maps (console script)
        yonosplat)   entry='python -m src.main "$@"' ;;            # Hydra: +experiment=... mode=test over a dataset index (see EVALUATION.md)
        spfsplatv2)  entry='python -m src.main "$@"' ;;            # Hydra: +experiment=... mode=test over RE10K/ACID data (see DATASETS.md)
        *)           entry='python demo.py "$@"' ;;
    esac
    cat > "$wrapper" <<WRAP
#!/bin/bash
# Auto-generated by setup_feedforward_sota.sh — runs $name.
SCRIPT_DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
DIR="\$SCRIPT_DIR/tools/$name"
# shellcheck disable=SC1091
source "\$DIR/venv/bin/activate"
cd "\$DIR/repo"
$entry
WRAP
    chmod +x "$wrapper"
    log "Wrapper: $wrapper"
    case "$cat" in
        geometry-colmap)
            log "  -> exports a COLMAP 'sparse/'; feed it to pipeline.sh --skip-colmap" ;;
        geometry-ply)
            log "  -> writes a PLY point cloud; melkor scene.ply scene.spz for the viewer" ;;
        geometry-mono)
            log "  -> per-image geometry (points/depth/normals); see docs/FEEDFORWARD_SOTA.md" ;;
        dataset-eval)
            log "  -> research/eval tool (Hydra + a dataset index), NOT folder-of-images;" \
                "see the repo's EVALUATION.md / DATASETS.md" ;;
    esac
}

for t in $TARGETS; do
    line="$(catalog | awk -F'|' -v s="$t" '$1==s')"
    IFS='|' read -r name repo cat lic note <<< "$line"
    install_one "$name" "$repo" "$cat" "$lic" "$note"
done

log ""
log "Done. Installed wrappers are in $PROJECT_ROOT (e.g. ./vggt-infer)."
log "COLMAP-exporting models feed pipeline.sh / opensplat_wrapper.sh directly."
log "See docs/FEEDFORWARD_SOTA.md for per-model I/O and pipeline wiring."
