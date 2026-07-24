# Resource limits

Melkor treats every file as untrusted, and "untrusted" includes *well-formed but enormous*. A
40 GiB PLY that declares two billion splats is not malformed. It is larger than the machine can
process safely. Without a limit, one file can exhaust memory, fill a disk, or stop a machine.
Thus, resource exhaustion is **in scope**. This policy reverses the pre-v2 policy under release
blocker P0-12.

Every limit is enforced through a shared `Budget` (`include/melkor/budget.hpp`), charged before
the allocation it accounts for. Thus, a new parser cannot omit the check.

## Profiles

Choose a named profile with `--limits-profile web|desktop|server`. The numbers below are safety
defaults, not scientific facts. The project will compare them with benchmark data before the
final release. Each change needs a changelog entry because it affects accepted inputs.

| Limit | web | desktop | server |
|---|---:|---:|---:|
| Input bytes | 2 GiB | 4 GiB | 32 GiB |
| Decoded bytes | 2 GiB | 8 GiB | 64 GiB |
| Working memory | 1 GiB | 4 GiB | 16 GiB |
| Splats | 8 M | 25 M | 150 M |
| Mesh triangles | 16 M | 50 M | 300 M |
| Decompression ratio | 100 | 1000 | 1000 |
| Image dimension | 8192 | 16384 | 32768 |
| Threads | 4 | logical CPUs (≤32) | logical CPUs (≤32) |

The `web` profile is the tightest because a browser tab cannot swap and exceeding memory kills the
page. `server` is still bounded — "server" means the operator chose these numbers knowingly, not
"unlimited".

## No "disable all limits" switch

There is deliberately none. A custom profile may raise a limit, but checked arithmetic, structural
format limits, path containment, and output-integrity safeguards always stay on. Melkor rejects a
limit that its arithmetic cannot represent. A zero limit does not mean "unlimited." It fails
validation because an all-zero profile can disable resource accounting accidentally.

## Decompression bombs

An absolute decoded-byte cap is not sufficient. It lets a 1 KiB file expand to the complete cap.
Before inflation, Melkor checks the declared expansion against a ratio guard
(`declared_decoded > max_ratio · compressed`). Thus, it rejects the *shape* and size of a bomb. A
valid, highly compressible asset can trigger the guard and needs an explicit override.

## Diagnostics

A limit failure exits with code **6**. Its diagnostic names the limit, observed value, and
applicable override flag. Thus, the message explains how to continue.
