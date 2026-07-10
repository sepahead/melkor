# DA3 CoreML research port

This directory contains an experimental Swift/CoreML port of parts of Depth
Anything 3 for macOS 14+ and iOS 17+. It is retained for parity work and
single-image depth experiments; it is **not** Melkor's supported reconstruction
backend.

## Current status

The current backbone export is fixed to a single image (`S=1`) and has not
passed numerical parity against the official PyTorch sequence model. It does
not preserve DA3's joint multi-view camera-token context. Consequently:

- `infer` and `benchmark` are the only registered CLI commands;
- multi-view fusion, streaming, model conversion, and 3DGS export are disabled;
- the setup/build/download/conversion helper scripts exit with status 2; and
- results must not be described as production DA3 reconstruction.

The old command implementations remain in the source as research material, but
they are not part of the executable command surface. Use the supported CUDA
bridge for reconstruction:

```bash
../../scripts/setup_da3.sh
../../da3-infer --input images/ --output scene.ply
```

## What remains testable

Maintainers with already converted, locally verified CoreML artifacts can build
the Swift package and exercise single-image depth inference:

```bash
swift package resolve
swift build -c release
swift test

.build/release/da3-coreml infer \
  --backbone /path/to/backbone.mlmodelc \
  --head /path/to/dualdpt-head.mlmodelc \
  --output ./output \
  image.jpg
```

The CLI can also benchmark the same pair of artifacts:

```bash
.build/release/da3-coreml benchmark \
  --backbone /path/to/backbone.mlmodelc \
  --head /path/to/dualdpt-head.mlmodelc
```

No public helper in this repository currently produces an artifact pair that
Melkor certifies as numerically equivalent to the reviewed upstream checkpoint.
Do not bypass the disabled scripts merely to obtain a runnable model.

## Re-enablement gates

The disabled surfaces may return only after all of these are satisfied:

1. Every supported checkpoint tensor is covered and provenance-pinned.
2. CoreML and official PyTorch outputs pass agreed absolute/relative tolerances
   for depth, confidence, rays, poses, and learned Gaussian fields.
3. Multi-view tests use `S > 1` and verify a shared world coordinate frame.
4. Camera-Z unprojection, intrinsics/extrinsics conventions, quaternion order,
   scale space, opacity space, and SH ordering match the official exporter.
5. Model conversion is deterministic, fail-closed, and validates the produced
   artifact before installation.
6. End-to-end fixtures cover inference, export, reload, and rendering.
7. Checkpoint licenses and all packaged third-party notices are recorded.

Until then, the authoritative DA3 integration is documented in
[`../../docs/DA3_FEEDFORWARD.md`](../../docs/DA3_FEEDFORWARD.md).

## Dependency policy

The sole Swift package dependency is pinned exactly and recorded in
`Package.resolved`. Update both only after `swift build` and `swift test` pass.
CoreML model files are deliberately not committed.

## License

The vendored Depth Anything 3 source is Apache-2.0; see `../LICENSE`. Model
weights can have different terms. Melkor's MIT license does not override those
terms.
