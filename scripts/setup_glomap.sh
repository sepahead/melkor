#!/bin/bash
set -euo pipefail

# ============================================================================
# DEPRECATED: standalone GLOMAP setup has been removed.
# ============================================================================
#
# Global structure-from-motion is now a first-class command inside COLMAP:
#
#     colmap global_mapper
#
# The separate GLOMAP program is obsolete, and this script used to clone it from a moving
# upstream branch -- a non-reproducible install of a deprecated tool (release blocker P0-14,
# and the mutable-clone half of P0-13). It no longer does that.
#
# See docs/migrations/2.0-glomap-to-colmap-global.md for the before/after commands. The pinned,
# reproducible COLMAP global-mapper adapter is delivered by the adapter runner (work package 18).

echo "setup_glomap.sh is deprecated and no longer installs standalone GLOMAP." >&2
echo >&2
echo "Global SfM now lives in COLMAP itself. Confirm your COLMAP build has it:" >&2
echo "    colmap global_mapper --help" >&2
echo >&2
echo "Migration guide: docs/migrations/2.0-glomap-to-colmap-global.md" >&2
exit 1
