#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Error: setup_lichtfeld.sh is retired.

The old script changed system packages, cloned a mutable branch, and downloaded
an archive without a recorded digest. Melkor cannot verify that installation.

Install a reviewed LichtFeld-Studio revision in an isolated environment.
Then set MELKOR_LICHTFELD_BIN or use --lichtfeld PATH.
See docs/LICHTFELD_WRAPPER.md.
EOF
exit 2
