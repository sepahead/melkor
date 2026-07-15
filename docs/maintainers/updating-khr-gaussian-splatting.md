# Updating the pinned KHR_gaussian_splatting extension

`KHR_gaussian_splatting` is a Khronos **release-candidate** extension. Melkor pins an exact
revision (`third_party/specifications.lock.json`) so that an editorial or semantic change
upstream cannot silently alter Melkor's behaviour. This is the procedure for moving to a newer
revision. It is deliberately not automatic: a spec change can change output semantics, and that
must be a reviewed decision.

## Procedure

1. **Open a tracking issue** naming the old and new commit SHAs and the new upstream status
   (still RC, or ratified).
2. **Diff the prose and the schemas** between the two commits. Read every change, not just the
   schema.
3. **Classify each change** as: no semantic effect / new optional feature / changed required
   semantics / breaking. A changed required semantic or a breaking change means a new profile.
4. **Create a NEW profile ID** — `profiles/gltf/khr-gaussian-splatting-rc-<newshort>.json` —
   rather than editing the old one. An existing profile ID never changes meaning; consumers pin
   to it.
5. **Vendor the new files** under `third_party/specs/KHR_gaussian_splatting/<newshort>/` and
   update `third_party/specifications.lock.json` with the commit, date, status, and file hashes.
   Run `python3 tools/verify_third_party.py --check` to confirm the hashes.
6. **Regenerate any generated code or fixtures** derived from the schema.
7. **Run the old and the new conformance corpora.** The old corpus proves you did not
   accidentally change how the old profile behaves; the new proves the new profile is correct.
8. **Decide read/write compatibility explicitly** and record it: can Melkor still read the old
   profile? Does it write the new one by default?
9. **Update the capability output and docs** (`melkor formats`, the format reference) and attach
   the glTF Validator and any independent-implementation evidence.
10. **Release as at least a minor version** when output semantics change, with a migration note.

## Why a new profile instead of mutating the old one

If profile `khr-gaussian-splatting-rc-63770cc` silently started meaning something different, every
asset and every consumer that pinned it would be quietly reinterpreted. A new ID makes the change
visible and lets old and new coexist, which is the whole point of pinning.
