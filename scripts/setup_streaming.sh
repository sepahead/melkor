#!/usr/bin/env bash
set -euo pipefail

# Read-only catalog for streaming research tools.

catalog() {
    cat <<'EOF'
gaussian-slam  https://github.com/VladimirYugay/Gaussian-SLAM
splatam        https://github.com/spla-tam/SplaTAM
splat-slam     https://github.com/google-research/Splat-SLAM
3dgstream      https://github.com/SJoJoK/3DGStream
monogs         https://github.com/muskie82/MonoGS
4d-gs          https://github.com/hustvl/4DGaussians
videogs        https://github.com/AuthorityWang/VideoGS
gifstream      https://github.com/XDimLab/GIFStream
spacetime      https://github.com/oppo-us-research/SpacetimeGaussians
ex4dgs         https://github.com/juno181/Ex4DGS
EOF
}

case "${1:-}" in
    list|--list)
        printf 'Streaming research catalog:\n'
        catalog
        exit 0
        ;;
    --help|-h|"")
        cat <<EOF
Usage: $0 list

This command only prints the reviewed research catalog.
It does not clone or install external software.
See docs/STREAMING.md for format and license limits.
EOF
        exit 0
        ;;
    *)
        cat >&2 <<'EOF'
Error: automatic streaming-tool checkout is retired.

The former command cloned mutable branches and recursive submodules.
Install a reviewed revision in an isolated environment.
Use `setup_streaming.sh list` to print the catalog.
EOF
        exit 2
        ;;
esac
