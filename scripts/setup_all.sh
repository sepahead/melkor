#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Error: setup_all.sh is retired.

The old script installed system packages and mutable external source trees.
That process did not provide an auditable dependency boundary.

Build the native project with the commands in docs/QUICKSTART.md.
For external tools, use a reviewed installation and pass its executable path.
See docs/adapters/index.md and docs/PIPELINE.md.
EOF
exit 2
