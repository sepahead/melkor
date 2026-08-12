# Loss policy

A format conversion is honest only when it reports each lost feature. Converting degree-4 canonical
PLY to the degree-3 glTF profile drops coefficients. Flattening a scene graph loses hierarchy.
Quantizing data for SPZ introduces measurable error. These changes are not parse failures.
A successful conversion must still report them.

Every successful `melkor convert` command produces a **loss report**.
This rule includes a zero-loss report.
The report follows [`loss-report-v1`](../../schemas/loss-report-v1.schema.json).

`melkor convert` writes this JSON report after output staging and before the atomic commit.
Use the report only when the command returns exit status zero.
The command writes human-readable status text to stderr.

![Explicit convert loss policy. Info and warning losses continue. A severe loss requires approval. A fatal loss stops with exit code 4.](../../assets/diagrams/loss-policy.svg)

## Severities and the policy

| Severity | Meaning | Behavior |
|---|---|---|
| `info` | Representational change, no expected rendered difference | Recorded, passes. |
| `warning` | Measurable but usually acceptable, such as quantization within a published bound | Recorded, passes. |
| `severe` | Semantic data is removed or guessed, such as SH degree 4 → 3 | **Blocks the commit** unless the caller approves this exact loss code. |
| `fatal` | The target cannot represent the asset without violating an invariant | **Always blocks. Cannot be approved.** |

Approval uses an exact code: `--allow-loss LOSS_SH_DEGREE_TRUNCATED`.
The API and CLI do not provide a blanket approval flag.
A fatal loss or safety error cannot be approved.

## Losses are not errors

A malformed file, violated invariant, resource-limit failure, or unsupported required extension
is an **error**, not a loss. The loss policy cannot approve it. The policy governs
*representational* trade-offs in a valid conversion, not failures.

## Stable codes

The loss codes are stable machine identifiers. Examples include `LOSS_SH_DEGREE_TRUNCATED` and
`LOSS_QUANTIZATION_APPLIED`. Each code keeps the same meaning throughout the 2.x line.
The v1 schema lists every valid code. Add a schema version before you add a loss code.

The report records each approved code. A reviewer can see which losses the caller accepted.
