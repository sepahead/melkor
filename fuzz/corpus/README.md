# Fuzz seed corpus

These small, deterministic files seed the coverage-guided parser fuzzers and are replayed as
ordinary regression tests. They are intentionally exempted from the repository-wide 3D asset
ignore rules. Do not add large datasets here; minimize a reproducer before committing it.

Provenance and licensing:

- `ply/valid_rgb.ply` and `ply/valid_3dgs.ply` are handwritten Melkor test fixtures and are
  distributed under the repository's MIT license.
- `gltf_khr/minimal_degree1.glb` was generated deterministically by Melkor's
  `KHR_gaussian_splatting` writer from repository-authored numeric test data. It is distributed
  under the repository's MIT license.
- `glb/test_cube.glb` is the 1,664-byte Khronos glTF Sample Models `Box.glb` fixture, SHA-256
  `ed52f7192b8311d700ac0ce80644e3852cd01537e4d62241b9acba023da3d54e`. Cesium donated the
  model for glTF testing under the Creative Commons Attribution 4.0 International license:
  <https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/Box>.
- `spz/test_cube.spz` is a deterministic SPZ v3 conversion of that attributed Box fixture (the
  gzip timestamp is zero), so it is retained under the same Creative Commons Attribution 4.0
  terms.

The upstream Box asset and its SPZ conversion are included only as parser test inputs. Their
Creative Commons terms do not change the license of Melkor's source code.
