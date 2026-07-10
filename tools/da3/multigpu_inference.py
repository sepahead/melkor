#!/usr/bin/env python3
"""Fail-closed compatibility entry point for the retired DA3 view sharder.

Depth Anything 3 estimates camera poses and geometry jointly across all views
of one scene. Melkor's former ``torchrun`` wrapper divided those views between
ranks, ran unrelated monocular predictions, and concatenated the results as if
they shared a world coordinate frame. That output was geometrically invalid.

Upstream's supported multi-GPU evaluator distributes complete tasks/scenes;
it does not shard one scene's joint ``inference()`` call. A future replacement
may launch a batch of scene directories, keeping every scene intact on one GPU.
For one long scene, use upstream DA3-Streaming on a single GPU instead.
"""

from __future__ import annotations

import sys


def main() -> int:
    """Explain the safe migration path and return a failing status."""
    print(
        "Error: da3-infer-multigpu has been retired because view-sharded DA3 "
        "inference does not preserve joint poses or a shared coordinate frame.",
        file=sys.stderr,
    )
    print(
        "Use ./da3-infer for one scene. For long sequences, use the official "
        "DA3-Streaming pipeline. Multi-GPU support will return only as a "
        "batch-of-scenes launcher where each scene stays on one GPU.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
