#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Error: setup_opensplat.sh is retired.

The old script cloned a mutable branch and downloaded an archive without a
recorded digest. Melkor cannot verify that installation.

Install a reviewed OpenSplat revision in an isolated environment.
Then set MELKOR_OPENSPLAT_BIN or use --opensplat PATH.
See docs/OPENSPLAT_WRAPPER.md.
EOF
exit 2
