# Shipping-surface adversarial review — 2026-07-15

A 10-area adversarial review of the whole v2 shipping C++ surface (the parts the earlier
glTF-focused review did not cover), each finding independently verified by a second agent.
**18 confirmed findings; 13 fixed, 5 deferred with reasons.** Run: a parallel Workflow over
the PLY reader, the math oracle, the SPZ fork, the safety substrate, the scene model, the atomic
writer, the C ABI, the CLI, and coordinate frames.

## Fixed (commits e80c19b, 0768c6b, 7993fd0, 767ad17)

| # | sev | area | defect | fix |
|---|---|---|---|---|
| 1 | high | math-covariance | symmetric_eigen's Jacobi early-exit uses an ABSOLUTE off-diagonal threshold `off < 1e-30`  | FIXED e80c19b — Jacobi early-exit made relative to the Frobenius norm |
| 2 | high | spz-integration | unpackQuaternionSmallestThree computes sqrt(1 - sum_squares) with no clamp, so a crafted v | FIXED 0768c6b — finiteness scan rejects NaN/inf decode at the boundary |
| 3 | high | safety-substrate | The web profile sets max_temp_bytes = 0 to mean "no temp allowed", but Budget::consume int | FIXED 0768c6b — web max_temp_bytes bounded; validate() rejects 0 |
| 4 | high | cli-inspect | The KHR SplatData->GaussianCloud bridge copies canonical LINEAR scale straight into Gaussi | FIXED 7993fd0 — bridge converts linear->log scale, linear->logit opacity |
| 5 | medium | ply-reader | readFromBuffer places no bound on header size (the configured max_ply_header_bytes limit i | FIXED 0768c6b — readFromBuffer bounds the header scan |
| 6 | medium | math-covariance | affine_transform_gaussian gates singularity on an ABSOLUTE `|determinant(linear)| < 1e-18` | FIXED e80c19b — affine singularity gate made relative |
| 7 | medium | math-quat-activation | from_frame checks orthonormality but never checks handedness (determinant = +1), so a left | FIXED 7993fd0 — from_frame rejects a left-handed frame (MK1205) |
| 8 | medium | scene-model | SplatData::validate() re-checks positions/scales/opacities but omits the unit-quaternion ( | FIXED 7993fd0 — validate() now checks unit quaternion + SH finiteness |
| 9 | low | ply-reader | readFromFile hardcodes a 1 MiB header cap instead of the configured max_ply_header_bytes,  | FIXED 767ad17 — readFromFile uses max_ply_header_bytes |
| 10 | low | math-covariance | The 'not positive-semidefinite' rejection tolerance is `-1e-9 * max(|lambda_max|, |lambda_ | FIXED e80c19b — PSD tolerance tightened to 1e-12 |
| 11 | low | safety-substrate | Budget::remaining() returns 0 for an unlimited (limit==0) budget, contradicting the 0-mean | FIXED 767ad17 — remaining() returns UINT64_MAX for an unlimited limit |
| 12 | low | atomic-writer | The temporary filename is derived by adding 29 characters to the destination's filename, s | FIXED 767ad17 — temp filename clamped to NAME_MAX |
| 13 | low | c-abi | melkor_get_version only rejects struct_size==0, so a nonzero-but-too-small struct_size (1. | FIXED 767ad17 — reject struct_size < sizeof(struct_size) |

## Deferred (tracked, with reasons)

- **[medium] src/spz_encoder.cpp:466** (spz-integration): SPZ decode charges only the compressed input_bytes against the Budget; the decoded point count and multi-GB al — DEFERRED — SPZ decoded-size bound needs a header peek; coupled to the SPZ v4 re-pin (P0-09). Input size is already bounded; decode is capped by upstream at 10M points.
- **[low] src/ply_writer.cpp:840** (ply-reader): ASCII decode copies the entire data section into a std::string, which std::istringstream then copies again int — DEFERRED (LOW) — ASCII path copies the data section into a std::string (~2x peak); an in-place parse is an optimization, tracked with the PLY streaming rewrite (WP07).
- **[low] src/ply_writer.cpp:689** (ply-reader): Binary decode of a double property casts to float with no range check, silently producing +/-inf for out-of-fl — DEFERRED (LOW) — a binary f64 property out of float range casts to +/-inf; the ASCII path already guards it. Edge case on malformed input.
- **[low] /Users/torusprime/Development/sepahead-github/melkor/src/core/scene.cpp:156** (scene-model): A quaternion within 1e-3 of unit is accepted by is_unit and stored un-normalized; downstream to_matrix() assum — DEFERRED (LOW) — a quaternion within the 1e-3 unit tolerance is stored un-normalized; the glTF reader already renormalizes before create(). A general renormalize-in-create is a tolerance-policy change.
- **[low] src/io/atomic_writer.cpp:374** (atomic-writer): The overwrite=false / no-clobber guarantee is enforced only by a create-time existence check; commit()'s renam — DEFERRED (LOW) — the overwrite=false no-clobber is enforced at create time, not at rename; closing the create->rename window needs renameat2(RENAME_NOREPLACE)/renameatx_np, platform-specific.
