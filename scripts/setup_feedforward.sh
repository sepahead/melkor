#!/usr/bin/env bash

# Compatibility entry point for the retired native feedforward facade.
# The former script downloaded unrelated checkpoints but installed no
# model-correct adapter capable of producing Melkor GaussianCloud data.
set -euo pipefail

cat >&2 <<'EOF'
Error: scripts/setup_feedforward.sh has been retired.

The former setup installed Splatter-Image/MVSplat weights for a native
--feedforward facade that did not implement model-correct inference adapters.
No supported command could consume those downloads safely.

Use one of the maintained, pinned paths instead:
  scripts/setup_da3.sh                 Depth Anything 3 -> PLY
  scripts/setup_feedforward_sota.sh    reviewed alternative catalog

See docs/DA3_FEEDFORWARD.md and docs/FEEDFORWARD_SOTA.md.
EOF
exit 2
