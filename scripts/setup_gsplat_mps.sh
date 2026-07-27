#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Error: setup_gsplat_mps.sh is retired.

The old script cloned a mutable branch and installed unpinned Python packages.
Melkor cannot verify that environment.

Use a reviewed external environment for Metal experiments.
The Melkor pipeline does not claim a gsplat-MPS command contract.
EOF
exit 2
