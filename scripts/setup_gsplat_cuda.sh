#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Error: setup_gsplat_cuda.sh is retired.

The old script cloned a mutable branch and installed unpinned Python packages.
Melkor cannot verify that environment.

Install gsplat from a reviewed source and dependency lock.
Use the upstream command contract directly.
See docs/GSPLAT_CUDA.md.
EOF
exit 2
