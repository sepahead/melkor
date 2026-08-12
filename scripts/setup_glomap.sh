#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Error: setup_glomap.sh is retired.

COLMAP now provides global structure-from-motion through `global_mapper`.
The old script cloned a mutable standalone GLOMAP branch.
Melkor cannot verify that installation.

Install a reviewed COLMAP build in an isolated environment.
Confirm that it provides this command:
  colmap global_mapper --help

See docs/migrations/2.0-glomap-to-colmap-global.md.
EOF
exit 2
